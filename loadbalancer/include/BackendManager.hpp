#pragma once

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <deque>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <shared_mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static constexpr int BACKEND_POOL_MIN = 8;
static constexpr int BACKEND_POOL_MAX = 32;

enum class LbAlgorithm { ROUND_ROBIN, LEAST_CONNECTIONS };

// ─── BackendServer ────────────────────────────────────────────────────────────
//
// Managed via shared_ptr so Conn objects can safely outlive a deregistration —
// the server is only freed when the last active connection closes.
// pool is touched only from the event-loop thread (no lock needed on it).
// is_healthy and active_connections are atomic for cross-thread access.

struct BackendServer {
   std::string ip;
   int         port;
   sockaddr_in addr{};
   std::atomic<bool> is_healthy{true};
   std::atomic<bool> deregistered{false};      // permanent removal vs temp unhealthy
   std::atomic<int>  active_connections{0};
   std::atomic<uint64_t> total_connections{0};  // lifetime counter
   std::atomic<uint64_t> total_latency_us{0};   // cumulative microseconds
   std::deque<int>   pool; // idle fds — event-loop thread only

   BackendServer(std::string_view ip_addr, int p) : ip(ip_addr), port(p) {
      addr.sin_family = AF_INET;
      addr.sin_port   = htons(port);
      inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
   }

   BackendServer(const BackendServer &) = delete;
   BackendServer &operator=(const BackendServer &) = delete;

   int connect_fresh() const {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return -1;
      if (::connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
         close(fd);
         return -1;
      }
      return fd;
   }

   bool is_fd_alive(int fd) const {
      char buf;
      int n = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
      return n < 0 && errno == EAGAIN;
   }

   int acquire() {
      while (!pool.empty()) {
         int fd = pool.front();
         pool.pop_front();
         if (is_fd_alive(fd)) return fd;
         close(fd);
      }
      return connect_fresh();
   }

   void top_up() {
      while ((int)pool.size() < BACKEND_POOL_MIN &&
             (int)pool.size() < BACKEND_POOL_MAX) {
         int fd = connect_fresh();
         if (fd < 0) break;
         pool.push_back(fd);
      }
   }

   void inc_connections() noexcept {
      active_connections.fetch_add(1, std::memory_order_relaxed);
   }

   void dec_connections() noexcept {
      active_connections.fetch_sub(1, std::memory_order_relaxed);
   }
};

// ─── BackendManager ───────────────────────────────────────────────────────────
//
// Thread-safe: add_server / remove_server may be called from the control thread
// while next() / top_up_all() run on the event-loop thread.

class BackendManager {
   using ServerPtr = std::shared_ptr<BackendServer>;

   mutable std::shared_mutex        mu_;
   std::vector<ServerPtr>           servers_;
   std::atomic<size_t>              rr_index_{0};
   LbAlgorithm                      algo_{LbAlgorithm::ROUND_ROBIN};

public:
   void set_algorithm(LbAlgorithm algo) { algo_ = algo; }

   void add_server(std::string_view ip, int port) {
      std::unique_lock lock(mu_);
      for (auto &s : servers_) {
         if (s->ip == ip && s->port == port) {
            // Re-register: clear deregistered, mark healthy
            s->deregistered.store(false, std::memory_order_relaxed);
            s->is_healthy.store(true, std::memory_order_relaxed);
            return;
         }
      }
      servers_.push_back(std::make_shared<BackendServer>(ip, port));
   }

   // Set health status for a non-deregistered server (used by HealthChecker).
   void set_healthy(std::string_view ip, int port, bool healthy) {
      std::shared_lock lock(mu_);
      for (auto &s : servers_) {
         if (s->ip == ip && s->port == port &&
             !s->deregistered.load(std::memory_order_relaxed)) {
            s->is_healthy.store(healthy, std::memory_order_relaxed);
            return;
         }
      }
   }

   // Mark unhealthy so no new connections are routed to it.
   // The shared_ptr keeps it alive until every active Conn closes.
   // Returns false if the server wasn't found.
   bool remove_server(std::string_view ip, int port) {
      std::unique_lock lock(mu_);
      for (auto &s : servers_) {
         if (s->ip == ip && s->port == port) {
            s->deregistered.store(true, std::memory_order_relaxed);
            s->is_healthy.store(false, std::memory_order_relaxed);
            return true;
         }
      }
      return false;
   }

   // Remove servers that are deregistered AND fully drained.
   // Call from the event-loop thread after dec_connections().
   void purge_drained() {
      std::unique_lock lock(mu_);
      servers_.erase(
         std::remove_if(servers_.begin(), servers_.end(), [](const ServerPtr &s) {
            return s->deregistered.load(std::memory_order_relaxed) &&
                   s->active_connections.load(std::memory_order_relaxed) == 0;
         }),
         servers_.end());
   }

   // Returns a shared_ptr so the backend stays alive for the duration of
   // the connection even if remove_server() is called concurrently.
   ServerPtr next() noexcept {
      std::shared_lock lock(mu_);
      if (servers_.empty()) return nullptr;

      if (algo_ == LbAlgorithm::ROUND_ROBIN) {
         size_t start = rr_index_.fetch_add(1, std::memory_order_relaxed);
         for (size_t i = 0; i < servers_.size(); ++i) {
            auto &s = servers_[(start + i) % servers_.size()];
            if (s->is_healthy.load(std::memory_order_relaxed) &&
                !s->deregistered.load(std::memory_order_relaxed))
               return s;
         }
         return nullptr;
      }

      if (algo_ == LbAlgorithm::LEAST_CONNECTIONS) {
         ServerPtr best;
         int min_conn = INT_MAX;
         for (auto &s : servers_) {
            if (!s->is_healthy.load(std::memory_order_relaxed) ||
                s->deregistered.load(std::memory_order_relaxed)) continue;
            int c = s->active_connections.load(std::memory_order_relaxed);
            if (c < min_conn) { min_conn = c; best = s; }
         }
         return best;
      }

      return nullptr;
   }

   void top_up_all() {
      std::shared_lock lock(mu_);
      for (auto &s : servers_)
         if (s->is_healthy.load(std::memory_order_relaxed))
            s->top_up();
   }

   // Call fn(shared_ptr<BackendServer>) for each healthy, non-deregistered server.
   // Used by the event loop to kick off async pool warmup at startup.
   template <typename Fn>
   void for_each_healthy(Fn &&fn) {
      std::shared_lock lock(mu_);
      for (auto &s : servers_)
         if (s->is_healthy.load(std::memory_order_relaxed) &&
             !s->deregistered.load(std::memory_order_relaxed))
            fn(s);
   }

   // Safe to call from any thread — only reads atomics after shared lock.
   struct BackendMetric {
      std::string label;            // "ip:port"
      int         active_conns;
      uint64_t    total_conns;
      uint64_t    total_latency_us;
   };

   std::vector<BackendMetric> backend_metrics() const {
      std::shared_lock lock(mu_);
      std::vector<BackendMetric> out;
      out.reserve(servers_.size());
      for (auto &s : servers_)
         out.push_back({
            s->ip + ":" + std::to_string(s->port),
            s->active_connections.load(std::memory_order_relaxed),
            s->total_connections.load(std::memory_order_relaxed),
            s->total_latency_us.load(std::memory_order_relaxed),
         });
      return out;
   }

   // Returns a snapshot of (ip, port) for all non-deregistered servers.
   // Used by HealthChecker to know which servers to probe.
   std::vector<std::pair<std::string, int>> get_servers_snapshot() const {
      std::shared_lock lock(mu_);
      std::vector<std::pair<std::string, int>> out;
      for (auto &s : servers_) {
         if (!s->deregistered.load(std::memory_order_relaxed))
            out.emplace_back(s->ip, s->port);
      }
      return out;
   }

   // Called from event-loop thread after async connect completes.
   void pool_push(std::string_view ip, int port, int fd) {
      std::shared_lock lock(mu_);
      for (auto &s : servers_) {
         if (s->ip == ip && s->port == port) {
            if ((int)s->pool.size() < BACKEND_POOL_MAX)
               s->pool.push_back(fd);
            else
               close(fd);
            return;
         }
      }
      close(fd);  // server was removed while connect was in-flight
   }
};
