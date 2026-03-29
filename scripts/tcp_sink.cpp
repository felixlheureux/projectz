// High-performance TCP accept-and-discard backend for CPS load testing.
// Uses true SO_REUSEPORT architecture to eliminate thundering herd lock contention.

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <csignal>
#include <cstring>

static std::atomic<bool> running{true};

static void on_signal(int) { running.store(false, std::memory_order_relaxed); }

static void worker(int port, int thread_id) {
    // 1. EACH thread creates its OWN listen socket
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) return;

    // 2. EACH thread sets SO_REUSEPORT
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // 3. EACH thread binds and listens independently
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd);
        return;
    }
    listen(listen_fd, 65535);

    int efd = epoll_create1(EPOLL_CLOEXEC);

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(efd, EPOLL_CTL_ADD, listen_fd, &ev);

    constexpr int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];
    char buf[4096];

    while (running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(efd, events, MAX_EVENTS, 200);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (true) {
                    int cfd = accept4(listen_fd, nullptr, nullptr,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break; // EAGAIN
                    
                    // Immediately tell the kernel to send RST on close to bypass TIME_WAIT on the sink
                    ::linger linger_val{1, 0};
                    setsockopt(cfd, SOL_SOCKET, SO_LINGER, &linger_val, sizeof(linger_val));

                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    epoll_ctl(efd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    continue;
                }
                while (true) {
                    ssize_t r = read(fd, buf, sizeof(buf));
                    if (r <= 0) {
                        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                            epoll_ctl(efd, EPOLL_CTL_DEL, fd, nullptr);
                            close(fd);
                        }
                        break;
                    }
                }
            }
        }
    }

    close(efd);
    close(listen_fd);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: tcp_sink <port>\n";
        return 1;
    }
    int port = std::atoi(argv[1]);

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    int nthreads = 4; // Force 4 threads to match the proxy workers
    std::cout << "[tcp_sink] Listening on 127.0.0.1:" << port
              << " (" << nthreads << " threads, True SO_REUSEPORT)\n" << std::flush;

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int i = 0; i < nthreads; ++i) {
        threads.emplace_back(worker, port, i);
    }

    for (auto& t : threads) t.join();
    return 0;
}