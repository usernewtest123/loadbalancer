#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <map>
struct Connection {
    int client_fd;
    int backend_fd;
};
int main(){
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, 128);
    std::cout << "Load balancer listening on port 8080\n";

    std::vector<struct pollfd> fds;
    std::map<int, int> client_to_backend;
    std::map<int, int> backend_to_client;

    fds.push_back({listenfd, POLLIN, 0});
    while(true){
        int ret=poll(fds.data(),fds.size(),-1);
        if(ret<0)
        {
            perror("poll");
            break;
        }
        for(size_t i=0;i<fds.size();i++){
            if (fds[i].revents & POLLIN) {
                if(fds[i].fd==listenfd){
                    int client_fd=accept(listenfd,NULL,NULL);
                     std::cout << "New client connected\n";
                    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);

                    sockaddr_in backend{};
                    backend.sin_family = AF_INET;
                    backend.sin_port = htons(9001);
                    inet_pton(AF_INET, "127.0.0.1", &backend.sin_addr);
                    connect(backend_fd, (sockaddr*)&backend, sizeof(backend));

                    client_to_backend[client_fd] = backend_fd;
                    backend_to_client[backend_fd] = client_fd;
                    fds.push_back({client_fd, POLLIN, 0});
                    fds.push_back({backend_fd, POLLIN, 0});
                }else{
                    int src_fd=fds[i].fd;
                    int des_fd=-1;
                    if(client_to_backend.count(src_fd)){
                        des_fd=client_to_backend[src_fd];
                    }
                    if(backend_to_client.count(src_fd)){
                        des_fd=backend_to_client[src_fd];
                    }
                    if(des_fd==-1)continue;
                    char buf[4096];
                    int n = read(src_fd, buf, sizeof(buf));
                    if (n <= 0) {

                    std::cout << "Connection closed on fd " << src_fd << "\n";

                    // Close both sides
                    int des_fd = -1;
                    if(client_to_backend.count(src_fd)) {
                        des_fd = client_to_backend[src_fd];
                        backend_to_client.erase(des_fd);
                        close(des_fd);
                        client_to_backend.erase(src_fd);
                    } else if (backend_to_client.count(src_fd)) {
                        des_fd = backend_to_client[src_fd];
                        client_to_backend.erase(des_fd);
                        close(des_fd);
                        backend_to_client.erase(src_fd);
                    }
                    close(src_fd);

                    // Remove from fds vector
                    fds.erase(fds.begin() + i);
                    if (des_fd != -1) {
                        for(size_t j = 0; j < fds.size(); j++) {
                            if(fds[j].fd == des_fd){
                                fds.erase(fds.begin() + j);
                                break;
                            }
                        }
                    }

                    i--; 
                    continue;
}
                    write(des_fd, buf, n);

                }
        }

    }
    
}
}
