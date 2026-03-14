#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <iostream>
#include <map>
#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define LISTEN_PORT 8181
#define BACKEND_PORT 9001
#define BACKEND_IP "127.0.0.1"
std::map<int, int> client_to_backend;
std::map<int, int> backend_to_client;

void set_nonblocking(int sock) {
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
}
int main(){
    int listen_fd=socket(AF_INET, SOCK_STREAM, 0);
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
    while(true)
    {
        int n_ready=epoll_wait(epfd,events,MAX_EVENTS,-1);
        for(int i=0;i<n_ready;i++){
            int fd=events[i].data.fd;
            if(fd==listen_fd){
                 int client_fd = accept(listen_fd, nullptr, nullptr);
                 set_nonblocking(client_fd);

                 int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in backend{};
                backend.sin_family = AF_INET;
                backend.sin_port = htons(BACKEND_PORT);
                inet_pton(AF_INET, BACKEND_IP, &backend.sin_addr);
                  set_nonblocking(backend_fd);
                connect(backend_fd, (sockaddr*)&backend, sizeof(backend));
              

                epoll_event e1{};
                e1.events = EPOLLIN;
                e1.data.fd =client_fd ;
                epoll_event e2{};
                e2.events = EPOLLIN;
                e2.data.fd =backend_fd ;
                epoll_ctl(epfd, EPOLL_CTL_ADD, backend_fd, &e2);
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &e1);
                client_to_backend[client_fd]=backend_fd;
                backend_to_client[backend_fd]=client_fd;

                std::cout << "New client connected, backend assigned\n";
            }
            else if(events[i].events & EPOLLIN){
                //int srcfd=fd;
                int dstfd=-1;
                if(client_to_backend.count(fd)){
                    dstfd=client_to_backend[fd];

                }
                else if(backend_to_client.count(fd)){
                    dstfd=backend_to_client[fd];
                }
                if(dstfd==-1)continue;
                int n = read(fd, buffer, sizeof(buffer));
                if(n<=0){
                    if(n==0){
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
        
                        continue;
                    } 
                        close(fd);
                        close(dstfd);
                         client_to_backend.erase(fd);
                        backend_to_client.erase(dstfd);
                        client_to_backend.erase(dstfd);
                        backend_to_client.erase(fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                            epoll_ctl(epfd, EPOLL_CTL_DEL, dstfd, nullptr);
                        std::cout << "Connection closed for "<<fd<<"and"<<dstfd<<"\n";
                    }
                }else{
                    write(dstfd, buffer, n);
                }
            }
        }
    }
    close(listen_fd);
}
