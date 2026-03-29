#include "BackendManager.hpp"
#include "ControlServer.hpp"
#include "HealthChecker.hpp"
#include "MetricsServer.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <iostream>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static constexpr unsigned RING_SIZE  = 256;
static constexpr size_t SPLICE_SIZE  = 65536;
static constexpr int QUEUE_MAX       = 1024;
static constexpr int CONCURRENCY_MAX = 10000;

// ─── data model ──────────────────────────────────────────────────────────────

enum class OpKind : uint8_t { FILL, DRAIN, POOL_CONNECT };

struct Conn {
   int            client_fd{-1};
   int            backend_fd{-1};
   int            c2b[2]{-1, -1};
   int            b2c[2]{-1, -1};
   bool           dead{false};
   int            pending{2};
   std::shared_ptr<BackendServer> backend{};
   std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};
};

struct Op {
   OpKind  kind;
   Conn   *conn;          // nullptr for POOL_CONNECT
   int     src_fd;
   int     dst_fd;
   int     pipe_rd;
   int     pipe_wr;
   ssize_t n{0};
   // For POOL_CONNECT:
   std::shared_ptr<BackendServer> backend{};
   sockaddr_in connect_addr{};
};

// ─── globals ─────────────────────────────────────────────────────────────────

static struct io_uring ring;
static int listen_fd = -1;
static BackendManager backends;

static std::deque<int>        wait_queue;
static std::atomic<int>       active_conns{0};
static std::atomic<int>       queue_depth{0};
static bool                   accept_armed = false;

static std::atomic<uint64_t>  connections_total{0};
static std::atomic<uint64_t>  errors_total{0};
static std::atomic<bool>      shutting_down{false};

// ─── helpers ─────────────────────────────────────────────────────────────────

void close_rst(int &fd) {
   if (fd < 0) return;
   struct linger l{1, 0};
   setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
   close(fd);
   fd = -1;
}

// ─── io_uring submission helpers ─────────────────────────────────────────────

void submit_fill(Op *op) {
   struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
   io_uring_prep_splice(sqe, op->src_fd, -1, op->pipe_wr, -1,
                        SPLICE_SIZE, SPLICE_F_MOVE);
   sqe->flags |= IOSQE_ASYNC;
   io_uring_sqe_set_data(sqe, op);
   op->kind = OpKind::FILL;
}

void submit_drain(Op *op) {
   struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
   io_uring_prep_splice(sqe, op->pipe_rd, -1, op->dst_fd, -1,
                        op->n, SPLICE_F_MOVE);
   io_uring_sqe_set_data(sqe, op);
   op->kind = OpKind::DRAIN;
}

static struct sockaddr_in s_accept_addr{};
static socklen_t          s_accept_len = sizeof(s_accept_addr);

void arm_accept() {
   if (accept_armed) return;
   struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
   io_uring_prep_accept(sqe, listen_fd,
                        (struct sockaddr *)&s_accept_addr, &s_accept_len, 0);
   io_uring_sqe_set_data(sqe, nullptr);
   io_uring_submit(&ring);
   accept_armed = true;
}

// Submit async connects to warm up a backend's idle pool.
// Replaces blocking top_up() — connections complete as POOL_CONNECT CQEs.
void async_top_up(std::shared_ptr<BackendServer> backend) {
   int needed = BACKEND_POOL_MIN - static_cast<int>(backend->pool.size());
   if (needed <= 0) return;

   bool submitted = false;
   for (int i = 0; i < needed; ++i) {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) break;

      Op *op          = new Op{};
      op->kind        = OpKind::POOL_CONNECT;
      op->conn        = nullptr;
      op->src_fd      = fd;
      op->backend     = backend;
      op->connect_addr = backend->addr;

      struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
      io_uring_prep_connect(sqe, fd,
                            reinterpret_cast<sockaddr *>(&op->connect_addr),
                            sizeof(op->connect_addr));
      io_uring_sqe_set_data(sqe, op);
      submitted = true;
   }
   if (submitted) io_uring_submit(&ring);
}

// ─── dispatch & queue ────────────────────────────────────────────────────────

bool dispatch(int client_fd) {
   auto backend = backends.next();   // shared_ptr<BackendServer>
   if (!backend) return false;

   int backend_fd = backend->acquire();
   if (backend_fd < 0) return false;

   async_top_up(backend);
   backend->inc_connections();

   Conn *conn      = new Conn();
   conn->client_fd  = client_fd;
   conn->backend_fd = backend_fd;
   conn->backend    = backend;
   pipe(conn->c2b);
   pipe(conn->b2c);

   Op *c2b = new Op{ OpKind::FILL, conn, client_fd,  backend_fd, conn->c2b[0], conn->c2b[1] };
   Op *b2c = new Op{ OpKind::FILL, conn, backend_fd, client_fd,  conn->b2c[0], conn->b2c[1] };

   submit_fill(c2b);
   submit_fill(b2c);
   io_uring_submit(&ring);

   connections_total.fetch_add(1, std::memory_order_relaxed);

   std::cout << "Proxying: client_fd=" << client_fd
             << " -> " << backend->ip << ":" << backend->port
             << " active=" << active_conns
             << " queued=" << wait_queue.size() << "\n";
   return true;
}

void op_done(Op *op) {
   Conn *conn = op->conn;
   delete op;
   if (--conn->pending > 0) return;

   if (conn->backend) {
      // Record connection duration for latency metrics.
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(
         std::chrono::steady_clock::now() - conn->start_time).count();
      conn->backend->total_latency_us.fetch_add(
         static_cast<uint64_t>(us), std::memory_order_relaxed);
      conn->backend->total_connections.fetch_add(1, std::memory_order_relaxed);

      conn->backend->dec_connections();
      backends.purge_drained();
   }
   close(conn->c2b[0]); close(conn->c2b[1]);
   close(conn->b2c[0]); close(conn->b2c[1]);
   delete conn;
   active_conns--;

   // drain one queued client now that a slot freed
   while (!wait_queue.empty()) {
      int queued_fd = wait_queue.front();
      wait_queue.pop_front();
      queue_depth--;
      if (dispatch(queued_fd)) { active_conns++; break; }
      close_rst(queued_fd);
   }

   if (!accept_armed && !shutting_down.load() && (int)wait_queue.size() < QUEUE_MAX)
      arm_accept();
}

void begin_teardown(Op *op) {
   Conn *conn = op->conn;
   if (conn->dead) { op_done(op); return; }
   conn->dead = true;
   close_rst(conn->client_fd);
   close_rst(conn->backend_fd);
   op_done(op);
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char const *argv[]) {
   // Usage: ./lb <port> <metrics_port> <control_port> [<backend_host> <backend_port> ...]
   if (argc < 4) {
      std::cerr << "Usage: " << argv[0]
                << " <port> <metrics_port> <control_port> [<backend_host> <backend_port> ...]\n";
      return 1;
   }

   int port         = std::stoi(argv[1]);
   int metrics_port = std::stoi(argv[2]);
   int control_port = std::stoi(argv[3]);
   for (int i = 4; i + 1 < argc; i += 2)
      backends.add_server(argv[i], std::stoi(argv[i + 1]));

   // ── SIGTERM / SIGINT → graceful shutdown ──────────────────────────────────
   struct sigaction sa{};
   sa.sa_handler = [](int) { shutting_down.store(true, std::memory_order_relaxed); };
   sigaction(SIGTERM, &sa, nullptr);
   sigaction(SIGINT,  &sa, nullptr);

   // ── listen socket ─────────────────────────────────────────────────────────
   listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   int opt = 1;
   setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   struct sockaddr_in server_addr{};
   server_addr.sin_family      = AF_INET;
   server_addr.sin_addr.s_addr = INADDR_ANY;
   server_addr.sin_port        = htons(port);
   bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
   listen(listen_fd, 128);

   // ── io_uring: try SQPOLL (needs CAP_SYS_NICE), fall back to default ───────
   {
      struct io_uring_params params{};
      params.flags          = IORING_SETUP_SQPOLL;
      params.sq_thread_idle = 10000; // park SQ thread after 10 ms idle

      if (io_uring_queue_init_params(RING_SIZE, &ring, &params) < 0) {
         std::cerr << "Info: IORING_SETUP_SQPOLL unavailable (needs CAP_SYS_NICE), "
                      "falling back to normal io_uring\n";
         if (io_uring_queue_init(RING_SIZE, &ring, 0) < 0) {
            std::cerr << "Error: io_uring_queue_init failed\n";
            return 1;
         }
      } else {
         std::cout << "io_uring: SQPOLL enabled\n";
      }
   }

   // ── warm up connection pools ──────────────────────────────────────────────
   // Kick off async connects for all statically configured backends.
   // Dynamically registered backends are warmed in dispatch() after first use.
   backends.for_each_healthy([](auto s) { async_top_up(s); });

   // ── auxiliary servers ─────────────────────────────────────────────────────
   MetricsServer metrics(metrics_port, [&]() -> LbMetrics {
      return {
         active_conns.load(std::memory_order_relaxed),
         queue_depth.load(std::memory_order_relaxed),
         connections_total.load(std::memory_order_relaxed),
         errors_total.load(std::memory_order_relaxed),
         backends.backend_metrics(),
      };
   });
   metrics.start();

   ControlServer control(control_port,
      [](std::string_view ip, int p) {
         backends.add_server(ip, p);
         std::cout << "Registered backend " << ip << ":" << p << "\n";
      },
      [](std::string_view ip, int p) {
         backends.remove_server(ip, p);
         std::cout << "Deregistered backend " << ip << ":" << p << "\n";
      });
   control.start();

   HealthChecker health(backends);
   health.start();

   std::cout << "Load balancer listening on port " << port
             << " metrics=:" << metrics_port
             << " control=:" << control_port << "\n";

   arm_accept();

   // ── event loop ────────────────────────────────────────────────────────────
   // Use a 100 ms timeout so SIGTERM can be detected even while idle.
   struct __kernel_timespec ts{0, 100'000'000};

   while (true) {
      struct io_uring_cqe *cqe;
      int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);

      if (ret == -ETIME || ret == -EINTR) {
         if (shutting_down.load() && active_conns.load() == 0) break;
         continue;
      }
      if (ret < 0) break;

      Op  *op  = static_cast<Op *>(io_uring_cqe_get_data(cqe));
      int  res = cqe->res;
      io_uring_cqe_seen(&ring, cqe);

      // ── POOL_CONNECT ──────────────────────────────────────────────────────
      if (op && op->kind == OpKind::POOL_CONNECT) {
         if (res == 0 && op->backend &&
             static_cast<int>(op->backend->pool.size()) < BACKEND_POOL_MAX) {
            op->backend->pool.push_back(op->src_fd);
         } else {
            close(op->src_fd);
         }
         delete op;
         continue;
      }

      // ── accept ────────────────────────────────────────────────────────────
      if (op == nullptr) {
         accept_armed = false;
         if (res < 0) {
            if (!shutting_down.load()) arm_accept();
            continue;
         }

         int client_fd = res;

         if (shutting_down.load()) {
            // Graceful shutdown: reject new clients, let active drain.
            close_rst(client_fd);
            continue;
         }

         if (active_conns < CONCURRENCY_MAX) {
            if (dispatch(client_fd)) {
               active_conns++;
            } else if ((int)wait_queue.size() < QUEUE_MAX) {
               wait_queue.push_back(client_fd);
               queue_depth++;
            } else {
               close_rst(client_fd);
            }
         } else if ((int)wait_queue.size() < QUEUE_MAX) {
            wait_queue.push_back(client_fd);
            queue_depth++;
         } else {
            close_rst(client_fd);
         }

         if ((int)wait_queue.size() < QUEUE_MAX) arm_accept();
         continue;
      }

      // ── fill / drain ──────────────────────────────────────────────────────
      if (res <= 0) {
         if (res < 0) errors_total.fetch_add(1, std::memory_order_relaxed);
         begin_teardown(op);
         continue;
      }

      if (op->kind == OpKind::FILL) {
         op->n = res;
         submit_drain(op);
      } else {
         submit_fill(op);
      }
      io_uring_submit(&ring);
   }

   // ── shutdown ──────────────────────────────────────────────────────────────
   std::cout << "Shutting down gracefully\n";
   health.stop();
   io_uring_queue_exit(&ring);
   close(listen_fd);
   return 0;
}
