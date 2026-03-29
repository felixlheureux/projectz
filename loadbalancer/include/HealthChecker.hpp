#pragma once

#include "BackendManager.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <map>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// Periodic TCP health-probe thread.
//
// Every PROBE_INTERVAL_S seconds, connects (non-blocking) to each registered
// backend.  After FAILURE_THRESHOLD consecutive failures the backend is marked
// unhealthy and will receive no new traffic.  After SUCCESS_THRESHOLD
// consecutive successes it is marked healthy again.

class HealthChecker {
    static constexpr int PROBE_INTERVAL_S  = 5;
    static constexpr int FAILURE_THRESHOLD = 3;
    static constexpr int SUCCESS_THRESHOLD = 2;
    static constexpr int PROBE_TIMEOUT_S   = 2;

    BackendManager   &backends_;
    std::thread       thread_;
    std::atomic<bool> stop_{false};

    // Non-blocking TCP connect with a select() timeout.
    bool probe(const std::string &ip, int port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return false;

        bool ok = false;
        int  r  = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        if (r == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv{PROBE_TIMEOUT_S, 0};
            r = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (r == 1) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                ok = (err == 0);
            }
        }

        struct linger l{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        close(fd);
        return ok;
    }

    void run() {
        std::map<std::string, int> fail_streak;
        std::map<std::string, int>  ok_streak;

        while (!stop_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(PROBE_INTERVAL_S));
            if (stop_.load(std::memory_order_relaxed)) break;

            for (auto &[ip, port] : backends_.get_servers_snapshot()) {
                std::string key = ip + ":" + std::to_string(port);
                if (probe(ip, port)) {
                    fail_streak[key] = 0;
                    if (++ok_streak[key] >= SUCCESS_THRESHOLD)
                        backends_.set_healthy(ip, port, true);
                } else {
                    ok_streak[key] = 0;
                    if (++fail_streak[key] >= FAILURE_THRESHOLD) {
                        backends_.set_healthy(ip, port, false);
                        // reset so recovery is detected from a clean slate
                        fail_streak[key] = FAILURE_THRESHOLD;
                    }
                }
            }
        }
    }

public:
    explicit HealthChecker(BackendManager &backends) : backends_(backends) {}

    void start() {
        thread_ = std::thread([this] { run(); });
        thread_.detach();
    }

    void stop() { stop_.store(true, std::memory_order_relaxed); }
};
