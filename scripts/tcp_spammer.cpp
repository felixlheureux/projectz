#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>

std::atomic<uint64_t> successful_connections{0};
std::atomic<uint64_t> failed_binds{0};
std::atomic<uint64_t> failed_connects{0};
std::atomic<uint64_t> epoll_errors{0};
std::atomic<bool>     is_running{true};

void worker_thread(int port, int concurrent_connections, int thread_idx) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = 0;
    src.sin_addr.s_addr = htonl(INADDR_LOOPBACK + static_cast<uint32_t>(thread_idx));

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return;

    std::unordered_set<int> active_fds;
    std::vector<epoll_event> events(concurrent_connections);

    auto spawn = [&]() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        
        if (bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) < 0) {
            failed_binds.fetch_add(1, std::memory_order_relaxed);
            close(fd);
            return;
        }
        
        if (connect(fd, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) < 0 && errno != EINPROGRESS) {
            failed_connects.fetch_add(1, std::memory_order_relaxed);
        }
        
        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            active_fds.insert(fd);
        } else {
            close(fd);
        }
    };

    for (int i = 0; i < concurrent_connections; ++i) spawn();

    while (is_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epoll_fd, events.data(), concurrent_connections, 10);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            
            if (err == 0 && !(ev & EPOLLERR) && !(ev & EPOLLHUP)) {
                successful_connections.fetch_add(1, std::memory_order_relaxed);
            } else {
                epoll_errors.fetch_add(1, std::memory_order_relaxed);
            }

            linger sl{1, 0};
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));

            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            active_fds.erase(fd);

            spawn();
        }
    }
    for (int fd : active_fds) close(fd);
    close(epoll_fd);
}

int main(int argc, char** argv) {
    int duration = 10;
    int port = 8080;
    
    rlimit rl{1000000, 1000000};
    setrlimit(RLIMIT_NOFILE, &rl); 

    int threads = 16; 
// Lower burst concurrency to prevent ListenDrops
    int conns_per_thread = 500; // 16 * 500 = 8000 total in-flight

    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i)
        workers.emplace_back(worker_thread, port, conns_per_thread, i);

    std::cout << "[TCP Spammer] Running...\n";
    for (int i = 0; i < duration; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    is_running.store(false);
    for (auto& t : workers) t.join();

    std::cout << "========================================\n"
              << "Successful Connections:   " << successful_connections.load() << "\n"
              << "Failed Binds (No Ports):  " << failed_binds.load() << "\n"
              << "Failed Connects (OS):     " << failed_connects.load() << "\n"
              << "Epoll Errors (RST/Drop):  " << epoll_errors.load() << "\n"
              << "========================================\n";
    return 0;
}