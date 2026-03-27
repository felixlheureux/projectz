// High-performance TCP accept-and-discard backend for CPS load testing.
// Uses the same edge-triggered epoll pattern as the proxy so it is never
// the bottleneck during a connection-rate benchmark.
//
// Usage: ./tcp_sink <port>

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

static void worker(int listen_fd) {
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
                    if (cfd < 0) break;
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
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: tcp_sink <port>\n";
        return 1;
    }
    int port = std::atoi(argv[1]);

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(lfd, 65535);

    int nthreads = static_cast<int>(std::thread::hardware_concurrency());
    if (nthreads < 1) nthreads = 1;

    std::cout << "[tcp_sink] Listening on 127.0.0.1:" << port
              << " (" << nthreads << " threads)\n" << std::flush;

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int i = 0; i < nthreads; ++i)
        threads.emplace_back(worker, lfd);

    for (auto& t : threads) t.join();
    close(lfd);
    return 0;
}
