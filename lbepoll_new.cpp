#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define LISTEN_PORT 8181
#define BACKEND_PORT 9001
#define BACKEND_IP "127.0.0.1"

// --- Connection Class ---
enum class ConnState { CONNECTING, ACTIVE, CLOSING };

struct Connection {
    int client_fd;
    int backend_fd;
    ConnState state;

    std::vector<char> client_to_backend_buf;
    std::vector<char> backend_to_client_buf;

    Connection(int cfd, int bfd)
        : client_fd(cfd), backend_fd(bfd), state(ConnState::CONNECTING) {}

    void close_connection(int epfd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, nullptr);
        epoll_ctl(epfd, EPOLL_CTL_DEL, backend_fd, nullptr);
        close(client_fd);
        close(backend_fd);
        state = ConnState::CLOSING;
        std::cout << "Connection closed for client " << client_fd
                  << " and backend " << backend_fd << "\n";
    }
};

// --- Globals ---
std::map<int, Connection*> fd_to_connection;

// --- Utility Functions ---
void set_nonblocking(int sock) {
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
}

// Try writing as much data as possible from buffer
void try_write(int fd, std::vector<char>& buffer) {
    while(!buffer.empty()) {
        int n = write(fd, buffer.data(), buffer.size());
        if(n > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + n);
        } else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // Cannot write more now
        } else {
            break; // Error or closed
        }
    }
}

int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);
    set_nonblocking(listen_fd);

    int epfd = epoll_create1(0);
    epoll_event ev, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    std::cout << "Epoll-based Load Balancer listening on port " << LISTEN_PORT << "\n";

    char buffer[BUFFER_SIZE];

    while (true) {
        int n_ready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n_ready; i++) {
            int fd = events[i].data.fd;

            // --- New client ---
            if (fd == listen_fd) {
                int client_fd = accept(listen_fd, nullptr, nullptr);
                if (client_fd < 0) continue;
                set_nonblocking(client_fd);

                int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
                set_nonblocking(backend_fd);

                sockaddr_in backend{};
                backend.sin_family = AF_INET;
                backend.sin_port = htons(BACKEND_PORT);
                inet_pton(AF_INET, BACKEND_IP, &backend.sin_addr);

                int ret = connect(backend_fd, (sockaddr*)&backend, sizeof(backend));
                if (ret < 0 && errno != EINPROGRESS) {
                    perror("Backend connect failed");
                    close(client_fd);
                    close(backend_fd);
                    continue;
                }

                // Create connection object
                Connection* conn = new Connection(client_fd, backend_fd);
                fd_to_connection[client_fd] = conn;
                fd_to_connection[backend_fd] = conn;

                // Add client socket to epoll
                epoll_event e1{};
                e1.events = EPOLLIN;
                e1.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &e1);

                // Add backend socket to epoll
                epoll_event e2{};
                e2.events = (ret == 0) ? EPOLLIN : EPOLLOUT; // Wait EPOLLOUT if connecting
                e2.data.fd = backend_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, backend_fd, &e2);

                std::cout << "New client connected, backend connect "
                          << ((ret == 0) ? "ready" : "in progress") << "\n";
            }

            // --- Backend connect completed (EPOLLOUT) ---
            else if (events[i].events & EPOLLOUT) {
                Connection* conn = fd_to_connection[fd];
                if (!conn || conn->state != ConnState::CONNECTING || fd != conn->backend_fd)
                    continue;

                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    std::cerr << "Backend connect failed, closing\n";
                    conn->close_connection(epfd);
                    fd_to_connection.erase(conn->client_fd);
                    fd_to_connection.erase(conn->backend_fd);
                    delete conn;
                    continue;
                }

                conn->state = ConnState::ACTIVE;

                // Modify backend epoll to read now
                epoll_event ev{};
                ev.events = EPOLLIN;
                ev.data.fd = conn->backend_fd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, conn->backend_fd, &ev);

                std::cout << "Backend connected for client " << conn->client_fd << "\n";
            }

            // --- EPOLLIN data ---
            else if (events[i].events & EPOLLIN) {

                Connection* conn = fd_to_connection[fd];
                if(!conn || conn->state != ConnState::ACTIVE)
                    continue;

                int n = read(fd, buffer, sizeof(buffer));

                if(n <= 0) {
                    conn->close_connection(epfd);
                    fd_to_connection.erase(conn->client_fd);
                    fd_to_connection.erase(conn->backend_fd);
                    delete conn;
                    continue;
                }

                if(fd == conn->client_fd) {
                    conn->client_to_backend_buf.insert(
                        conn->client_to_backend_buf.end(),
                        buffer,
                        buffer + n
                    );

                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLOUT;
                    ev.data.fd = conn->backend_fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, conn->backend_fd, &ev);

                } else {
                    conn->backend_to_client_buf.insert(
                        conn->backend_to_client_buf.end(),
                        buffer,
                        buffer + n
                    );

                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLOUT;
                    ev.data.fd = conn->client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, conn->client_fd, &ev);
                }
            }

            // --- EPOLLOUT data ---
            else if (events[i].events & EPOLLOUT) {

                Connection* conn = fd_to_connection[fd];
                if(!conn || conn->state != ConnState::ACTIVE)
                    continue;

                if(fd == conn->backend_fd) {

                    try_write(fd, conn->client_to_backend_buf);

                    if(conn->client_to_backend_buf.empty()) {
                        epoll_event ev{};
                        ev.events = EPOLLIN;
                        ev.data.fd = conn->backend_fd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, conn->backend_fd, &ev);
                    }

                } else {

                    try_write(fd, conn->backend_to_client_buf);

                    if(conn->backend_to_client_buf.empty()) {
                        epoll_event ev{};
                        ev.events = EPOLLIN;
                        ev.data.fd = conn->client_fd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, conn->client_fd, &ev);
                    }
                }
            }
        }
    }

    close(listen_fd);
}
