#pragma once

#include "BackendManager.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <functional>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct LbMetrics {
    int      active_conns;
    int      queue_depth;
    uint64_t connections_total;
    uint64_t errors_total;
    std::vector<BackendManager::BackendMetric> backends;
};

// Minimal HTTP/1.1 server on a background thread.
// GET /metrics → Prometheus text format (used by KEDA / Prometheus scrape).
// connections_per_second is computed between successive scrapes.
class MetricsServer {
    int                          port_;
    std::function<LbMetrics()>   provider_;
    std::thread                  thread_;

    static std::string build_response(const LbMetrics &m, double cps) {
        std::ostringstream body;

        body << "# HELP lb_active_connections Active proxied connections\n"
             << "# TYPE lb_active_connections gauge\n"
             << "lb_active_connections " << m.active_conns << "\n\n"

             << "# HELP lb_queue_depth Clients waiting for a backend slot\n"
             << "# TYPE lb_queue_depth gauge\n"
             << "lb_queue_depth " << m.queue_depth << "\n\n"

             << "# HELP lb_connections_total Total accepted connections lifetime\n"
             << "# TYPE lb_connections_total counter\n"
             << "lb_connections_total " << m.connections_total << "\n\n"

             << "# HELP lb_errors_total Total connection errors lifetime\n"
             << "# TYPE lb_errors_total counter\n"
             << "lb_errors_total " << m.errors_total << "\n\n"

             << "# HELP lb_connections_per_second New connections per second\n"
             << "# TYPE lb_connections_per_second gauge\n"
             << "lb_connections_per_second " << cps << "\n\n"

             << "# HELP lb_backend_active_connections Active connections per backend\n"
             << "# TYPE lb_backend_active_connections gauge\n";
        for (auto &b : m.backends)
            body << "lb_backend_active_connections{backend=\"" << b.label << "\"} "
                 << b.active_conns << "\n";

        body << "\n# HELP lb_backend_connections_total Lifetime connections per backend\n"
             << "# TYPE lb_backend_connections_total counter\n";
        for (auto &b : m.backends)
            body << "lb_backend_connections_total{backend=\"" << b.label << "\"} "
                 << b.total_conns << "\n";

        body << "\n# HELP lb_backend_avg_latency_ms Average connection duration per backend\n"
             << "# TYPE lb_backend_avg_latency_ms gauge\n";
        for (auto &b : m.backends) {
            double avg_ms = (b.total_conns > 0)
                ? static_cast<double>(b.total_latency_us) / b.total_conns / 1000.0
                : 0.0;
            body << "lb_backend_avg_latency_ms{backend=\"" << b.label << "\"} "
                 << avg_ms << "\n";
        }

        std::string bs = body.str();
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/plain; version=0.0.4\r\n"
             << "Content-Length: " << bs.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << bs;
        return resp.str();
    }

    // Minimal JSON response for KEDA metrics-api trigger.
    // Returns {"lb_queue_depth":<n>,"lb_active_connections":<n>}
    static std::string build_json(const LbMetrics &m) {
        std::ostringstream body;
        body << "{\"lb_queue_depth\":" << m.queue_depth
             << ",\"lb_active_connections\":" << m.active_conns << "}";
        std::string b = body.str();
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << b.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << b;
        return resp.str();
    }

    void serve() {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);
        bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        listen(server_fd, 8);

        uint64_t prev_total = 0;
        auto     prev_time  = std::chrono::steady_clock::now();

        while (true) {
            int client = accept(server_fd, nullptr, nullptr);
            if (client < 0) continue;

            // Read the request line to distinguish /metrics from /metrics/json
            char buf[512]{};
            recv(client, buf, sizeof(buf) - 1, 0);

            std::string req(buf);
            bool want_json = req.find("GET /metrics/json") != std::string::npos;

            auto m = provider_();

            std::string resp;
            if (want_json) {
                resp = build_json(m);
            } else {
                auto   now     = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - prev_time).count();
                double cps     = (elapsed > 0.0)
                    ? static_cast<double>(m.connections_total - prev_total) / elapsed
                    : 0.0;
                prev_total = m.connections_total;
                prev_time  = now;
                resp = build_response(m, cps);
            }

            send(client, resp.c_str(), resp.size(), 0);
            close(client);
        }

        close(server_fd);
    }

public:
    MetricsServer(int port, std::function<LbMetrics()> provider)
        : port_(port), provider_(std::move(provider)) {}

    void start() {
        thread_ = std::thread([this] { serve(); });
        thread_.detach();
    }
};
