// The Global Module Fragment
module;

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>

export module ProxyServer;

import DnsResolver;
import ProxyWorker;

// Orchestrator: spawns N ProxyWorker threads (one per CPU core), each with its
// own epoll instance and listen socket bound to the same port via SO_REUSEPORT.
// The kernel distributes incoming connections across workers with zero contention.
export class ProxyServer {
private:
    static constexpr int TOTAL_MAX_FDS = 1000000;

    int port_;
    int num_workers_;
    std::shared_ptr<DnsResolver> dns_resolver_;
    std::vector<std::unique_ptr<ProxyWorker>> workers_;
    std::vector<std::thread> threads_;
    std::atomic<bool> is_running_{true};

public:
    ProxyServer(int port, std::string backend_hostname, int backend_port = 8080)
        : port_(port),
          num_workers_(static_cast<int>(std::thread::hardware_concurrency())),
          dns_resolver_(std::make_shared<DnsResolver>(std::move(backend_hostname), backend_port))
    {
        // Minimum 1 worker even if hardware_concurrency() returns 0
        if (num_workers_ < 1) num_workers_ = 1;

        // Divide FD space evenly across workers
        int fds_per_worker = TOTAL_MAX_FDS / num_workers_;

        workers_.reserve(num_workers_);
        for (int i = 0; i < num_workers_; ++i) {
            workers_.push_back(std::make_unique<ProxyWorker>(
                i, port_, dns_resolver_, is_running_, fds_per_worker
            ));
        }
    }

    ProxyServer(const ProxyServer&) = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;
    ProxyServer(ProxyServer&&) = delete;
    ProxyServer& operator=(ProxyServer&&) = delete;

    void run() noexcept {
        // Spawn a dedicated thread for each worker (one per core)
        threads_.reserve(num_workers_);
        for (auto& worker : workers_) {
            threads_.emplace_back([&worker]() {
                worker->run();
            });
        }

        // Block until all worker threads exit
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

    void stop() noexcept {
        is_running_.store(false, std::memory_order_release);
    }

    [[nodiscard]] int num_workers() const noexcept { return num_workers_; }
};
