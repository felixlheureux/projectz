#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <deque>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static constexpr int BACKEND_POOL_MIN = 8;
static constexpr int BACKEND_POOL_MAX = 32;

enum class LbAlgorithm { ROUND_ROBIN, LEAST_CONNECTIONS };

// ─── BackendServer ────────────────────────────────────────────────────────────
//
// One upstream node. Owns its own idle fd pool (touched only from the event
// loop thread, so no locking needed). is_healthy and active_connections are
// atomic so a future health-check thread can read/write them safely.

struct BackendServer {
   std::string ip;
   int port;
   sockaddr_in addr{};
   std::atomic<bool> is_healthy{true};
   std::atomic<int>  active_connections{0};
   std::deque<int>   pool; // idle fds — event-loop thread only

   BackendServer(std::string_view ip_addr, int p) : ip(ip_addr), port(p) {
      addr.sin_family = AF_INET;
      addr.sin_port   = htons(port);
      inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
   }

   // not copyable — atomics and fd ownership
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

   // EAGAIN on a non-blocking peek = alive with no stale data
   bool is_fd_alive(int fd) const {
      char buf;
      int n = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
      return n < 0 && errno == EAGAIN;
   }

   // Pop a healthy idle fd, or open a fresh one.
   int acquire() {
      while (!pool.empty()) {
         int fd = pool.front();
         pool.pop_front();
         if (is_fd_alive(fd)) return fd;
         close(fd);
      }
      return connect_fresh();
   }

   // Refill idle pool up to BACKEND_POOL_MIN.
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

class BackendManager {
   std::vector<std::unique_ptr<BackendServer>> servers_;
   std::atomic<size_t> rr_index_{0};
   LbAlgorithm algo_{LbAlgorithm::ROUND_ROBIN};

public:
   void add_server(std::string_view ip, int port) {
      servers_.push_back(std::make_unique<BackendServer>(ip, port));
   }

   void set_algorithm(LbAlgorithm algo) { algo_ = algo; }

   // Pick the next healthy backend according to the current algorithm.
   BackendServer *next() noexcept {
      if (servers_.empty()) return nullptr;

      if (algo_ == LbAlgorithm::ROUND_ROBIN) {
         size_t start = rr_index_.fetch_add(1, std::memory_order_relaxed);
         for (size_t i = 0; i < servers_.size(); ++i) {
            auto &s = servers_[(start + i) % servers_.size()];
            if (s->is_healthy.load(std::memory_order_relaxed))
               return s.get();
         }
         return nullptr;
      }

      if (algo_ == LbAlgorithm::LEAST_CONNECTIONS) {
         BackendServer *best     = nullptr;
         int            min_conn = INT_MAX;
         for (auto &s : servers_) {
            if (!s->is_healthy.load(std::memory_order_relaxed)) continue;
            int c = s->active_connections.load(std::memory_order_relaxed);
            if (c < min_conn) { min_conn = c; best = s.get(); }
         }
         return best;
      }

      return nullptr;
   }

   // Warm up every healthy backend's idle pool.
   void top_up_all() {
      for (auto &s : servers_)
         if (s->is_healthy.load(std::memory_order_relaxed))
            s->top_up();
   }
};
