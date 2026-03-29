// High-performance raw TCP CPS (Connections Per Second) load generator.
//
// Three deliberate design choices vs the naive approach:
//   1. RST teardown  — SO_LINGER{1,0} before close() sends RST instead of FIN,
//                      so ports exit TIME_WAIT instantly and are immediately
//                      reusable.  Without this, the ~64k ephemeral port range
//                      is exhausted at ~32k CPS on a single source IP.
//   2. Per-thread source IP — each thread binds to a distinct loopback address
//                      (127.0.0.1, 127.0.0.2, …).  Every IP gives another 64k
//                      ephemeral ports, multiplying the port budget by N threads.
//   3. 4 000 concurrent FDs per thread — scaled across 16 threads to fully
//                      pipeline the kernel TCP state machine safely in memory.
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
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

std::atomic<uint64_t> successful_connections{0};
std::atomic<bool>     is_running{true};

void worker_thread(int port, int concurrent_connections, int thread_idx) {
    // Destination: the proxy
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

    // Source: a distinct loopback IP per thread to multiply the ephemeral port
    // budget.  Linux owns the entire 127.0.0.0/8 range as loopback.
    sockaddr_in src{};
    src.sin_family      = AF_INET;
    src.sin_port        = 0; // OS assigns ephemeral port
    src.sin_addr.s_addr = htonl(INADDR_LOOPBACK + static_cast<uint32_t>(thread_idx));

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return;

    std::unordered_set<int> active_fds;
    active_fds.reserve(concurrent_connections * 2);
    std::vector<epoll_event> events(concurrent_connections);

    auto spawn = [&]() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        // Bind to the per-thread source IP before connecting so the kernel picks
        // an ephemeral port from the correct IP's range.
        if (bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) < 0) {
            std::cerr << "BIND FAILED! Port Exhaustion on IP " << thread_idx << "\n";
            close(fd);
            return;
        }
        connect(fd, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        epoll_event ev{};
        ev.events  = EPOLLOUT | EPOLLERR;
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

            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) successful_connections.fetch_add(1, std::memory_order_relaxed);

            // RST teardown: port exits TIME_WAIT immediately and can be reused
            // by the next spawn(), removing the 64k-port ceiling on CPS.
            linger sl{};
            sl.l_onoff  = 1;
            sl.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));

            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            active_fds.erase(fd);

            spawn(); // immediately replace with a fresh connection
        }
    }

    for (int fd : active_fds) close(fd);
    close(epoll_fd);
}

int main(int argc, char** argv) {
    int duration = 10;
    int port     = 8080;
    if (argc > 1) duration = std::atoi(argv[1]);
    if (argc > 2) port     = std::atoi(argv[2]);

    // Raise the per-process FD limit so 64k concurrent connections don't
    // exhaust the default 1024-socket limit. Requires the hard limit to have
    // been raised first (make tune / tune_kernel.sh sets it to 1 000 000).
    rlimit rl{1000000, 1000000};
    setrlimit(RLIMIT_NOFILE, &rl); 

    // [MODIFIED] Force 16 distinct source IPs to bypass the 262k total ephemeral port limit
    int threads = 16; 
    
    // [MODIFIED] 16 threads * 4000 FDs = 64,000 concurrent in-flight connections
    int conns_per_thread = 4000;

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int i = 0; i < threads; ++i)
        workers.emplace_back(worker_thread, port, conns_per_thread, i);

    std::cout << "[TCP Spammer] Targeting 127.0.0.1:" << port
              << " for " << duration << " seconds\n"
              << "[TCP Spammer] " << threads << " threads × "
              << conns_per_thread << " concurrent FDs  "
              << "(source IPs: 127.0.0.1 – 127.0.0." << threads << ")\n";

    for (int i = 0; i < duration; ++i) {
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