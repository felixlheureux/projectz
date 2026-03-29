#include "BackendManager.hpp"

#include <arpa/inet.h>
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

enum class OpKind : uint8_t { FILL, DRAIN };

struct Conn {
   int            client_fd{-1};
   int            backend_fd{-1};
   int            c2b[2]{-1, -1};
   int            b2c[2]{-1, -1};
   bool           dead{false};
   int            pending{2};
   BackendServer *backend{nullptr}; // for connection accounting
};

struct Op {
   OpKind  kind;
   Conn   *conn;
   int     src_fd;
   int     dst_fd;
   int     pipe_rd;
   int     pipe_wr;
   ssize_t n{0};
};

// ─── globals ─────────────────────────────────────────────────────────────────

static struct io_uring ring;
static int listen_fd = -1;
static BackendManager backends;

static std::deque<int> wait_queue;
static int  active_conns  = 0;
static bool accept_armed  = false;

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

// ─── dispatch & queue ────────────────────────────────────────────────────────

bool dispatch(int client_fd) {
   BackendServer *backend = backends.next();
   if (!backend) return false;

   int backend_fd = backend->acquire();
   backend->top_up();
   if (backend_fd < 0) return false;

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

   if (conn->backend) conn->backend->dec_connections();
   close(conn->c2b[0]); close(conn->c2b[1]);
   close(conn->b2c[0]); close(conn->b2c[1]);
   delete conn;
   active_conns--;

   // drain one queued client now that a slot freed
   while (!wait_queue.empty()) {
      int queued_fd = wait_queue.front();
      wait_queue.pop_front();
      if (dispatch(queued_fd)) { active_conns++; break; }
      close_rst(queued_fd);
   }

   if (!accept_armed && (int)wait_queue.size() < QUEUE_MAX)
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
   // Usage: ./lb <port> <backend_host> <backend_port> [<host> <port> ...]
   if (argc < 4 || (argc - 2) % 2 != 0) {
      std::cerr << "Usage: " << argv[0]
                << " <port> <backend_host> <backend_port> [<host> <port> ...]\n";
      return 1;
   }

   int port = std::stoi(argv[1]);
   for (int i = 2; i < argc; i += 2)
      backends.add_server(argv[i], std::stoi(argv[i + 1]));

   listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   int opt = 1;
   setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   struct sockaddr_in server_addr{};
   server_addr.sin_family      = AF_INET;
   server_addr.sin_addr.s_addr = INADDR_ANY;
   server_addr.sin_port        = htons(port);
   bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
   listen(listen_fd, 128);

   if (io_uring_queue_init(RING_SIZE, &ring, 0) < 0) {
      std::cerr << "Error: io_uring_queue_init failed\n";
      return 1;
   }

   backends.top_up_all();
   std::cout << "Load balancer listening on port " << port << "\n";

   arm_accept();

   while (true) {
      struct io_uring_cqe *cqe;
      if (io_uring_wait_cqe(&ring, &cqe) < 0) break;

      Op  *op  = static_cast<Op *>(io_uring_cqe_get_data(cqe));
      int  res = cqe->res;
      io_uring_cqe_seen(&ring, cqe);

      // ── accept ──────────────────────────────────────────────────────────
      if (op == nullptr) {
         accept_armed = false;
         if (res < 0) { arm_accept(); continue; }

         int client_fd = res;

         if (active_conns < CONCURRENCY_MAX) {
            if (dispatch(client_fd)) {
               active_conns++;
            } else if ((int)wait_queue.size() < QUEUE_MAX) {
               wait_queue.push_back(client_fd);
            } else {
               close_rst(client_fd);
            }
         } else if ((int)wait_queue.size() < QUEUE_MAX) {
            wait_queue.push_back(client_fd);
         } else {
            close_rst(client_fd);
         }

         if ((int)wait_queue.size() < QUEUE_MAX) arm_accept();
         continue;
      }

      // ── fill / drain ─────────────────────────────────────────────────────
      if (res <= 0) { begin_teardown(op); continue; }

      if (op->kind == OpKind::FILL) {
         op->n = res;
         submit_drain(op);
      } else {
         submit_fill(op);
      }
      io_uring_submit(&ring);
   }

   io_uring_queue_exit(&ring);
   close(listen_fd);
   return 0;
}
