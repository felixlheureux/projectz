// A high-performance, raw TCP connection spammer to measure CPS (Connections Per Second).
// Does not expect HTTP responses, making it ideal for testing L4 proxies that don't
// have a live backend configured yet.

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

std::atomic<uint64_t> successful_connections{0};
std::atomic<bool> is_running{true};

void worker_thread(int port, int concurrent_connections) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int epoll_fd = epoll_create1(0);
    std::vector<int> sockets(concurrent_connections, -1);
    std::vector<epoll_event> events(concurrent_connections);

    // Initial batch of non-blocking connects
    for (int i = 0; i < concurrent_connections; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        sockets[i] = fd;
        connect(fd, (struct sockaddr*)&addr, sizeof(addr));
        epoll_event ev;
        ev.events = EPOLLOUT | EPOLLERR;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    while (is_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epoll_fd, events.data(), concurrent_connections, 100);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            
            // Check if connection succeeded
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            
            if (error == 0) {
                successful_connections.fetch_add(1, std::memory_order_relaxed);
            }
            
            // Immediately close and reconnect to push purely CPS
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            
            int new_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            connect(new_fd, (struct sockaddr*)&addr, sizeof(addr));
            
            epoll_event ev;
            ev.events = EPOLLOUT | EPOLLERR;
            ev.data.fd = new_fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &ev);
        }
    }

    for(int fd : sockets) if(fd != -1) close(fd);
    close(epoll_fd);
}

int main(int argc, char** argv) {
    int port = 8080;
    int duration = 10;
    if (argc > 1) duration = std::atoi(argv[1]);
    
    int threads = std::thread::hardware_concurrency();
    int conns_per_thread = 1000;
    
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; i++) {
        workers.emplace_back(worker_thread, port, conns_per_thread);
    }
    
    std::cout << "[TCP Spammer] Saturating 127.0.0.1:" << port << " for " << duration << " seconds..." << std::endl;
    std::cout << "[TCP Spammer] " << threads << " threads × " << conns_per_thread << " concurrent FDs per thread" << std::endl;
    
    for (int i = 0; i < duration; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "." << std::flush;
    }
    std::cout << std::endl;
    
    is_running.store(false);
    for (auto& t : workers) t.join();
    
    uint64_t total = successful_connections.load();
    std::cout << "========================================" << std::endl;
    std::cout << "Total Connections Established: " << total << std::endl;
    std::cout << "Connections Per Second (CPS):  " << (total / duration) << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
