// High-performance raw TCP CPS (Connections Per Second) load generator.
// Measures how fast the proxy can accept and forward connections.
// Does not send payload data — this is a pure connection establishment benchmark.
//
// Usage: ./tcp_spammer <duration_seconds> <port>

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_set>
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

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return;

    // Track all live FDs so cleanup is correct even after reconnects.
    std::unordered_set<int> active_fds;
    std::vector<epoll_event> events(concurrent_connections);

    auto spawn = [&]() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLERR;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            return;
        }
        active_fds.insert(fd);
    };

    for (int i = 0; i < concurrent_connections; ++i) spawn();

    while (is_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epoll_fd, events.data(), concurrent_connections, 100);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error == 0) {
                successful_connections.fetch_add(1, std::memory_order_relaxed);
            }

            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            active_fds.erase(fd);

            // Immediately replace with a fresh connection to maintain pressure.
            spawn();
        }
    }

    for (int fd : active_fds) close(fd);
    close(epoll_fd);
}

int main(int argc, char** argv) {
    int duration = 10;
    int port = 8080;
    if (argc > 1) duration = std::atoi(argv[1]);
    if (argc > 2) port = std::atoi(argv[2]);

    int threads = static_cast<int>(std::thread::hardware_concurrency());
    if (threads < 1) threads = 1;
    int conns_per_thread = 1000;

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int i = 0; i < threads; i++) {
        workers.emplace_back(worker_thread, port, conns_per_thread);
    }

    std::cout << "[TCP Spammer] Targeting 127.0.0.1:" << port
              << " for " << duration << " seconds\n"
              << "[TCP Spammer] " << threads << " threads × "
              << conns_per_thread << " concurrent FDs per thread\n";

    for (int i = 0; i < duration; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "." << std::flush;
    }
    std::cout << "\n";

    is_running.store(false);
    for (auto& t : workers) t.join();

    uint64_t total = successful_connections.load();
    std::cout << "========================================\n"
              << "Total Connections:        " << total << "\n"
              << "Connections Per Second:   " << (total / duration) << "\n"
              << "========================================\n";

    return 0;
}
