// The Global Module Fragment
module;

#include <array>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <cerrno>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <atomic>

export module ProxyWorker;

import Socket;
import Epoll;
import FileDescriptor;
import DnsResolver;

// A single-threaded event loop worker. Each worker owns its own listen socket
// (bound to the shared port via SO_REUSEPORT), its own epoll instance, and its
// own connection pool. The kernel distributes incoming SYN packets across workers,
// guaranteeing zero contention between threads on the accept path.
export class ProxyWorker {
private:
    static constexpr int MAX_EVENTS = 1024;

    int worker_id_;
    int max_fds_;
    Epoll epoll_;
    Socket listen_sock_;
    std::shared_ptr<DnsResolver> dns_resolver_;
    std::atomic<bool>& is_running_;

    int max_connections_;        // hard cap: max_fds_ / 2 (2 fds per client+backend pair)
    int active_connections_{0};  // live pairs tracked by this worker

    size_t round_robin_index_{0};
    std::vector<std::optional<Socket>> client_sockets_;
    std::vector<int> socket_partners_;
    std::vector<std::vector<char>> write_buffers_;
    std::vector<bool> is_pending_connect_;

    // Set SO_LINGER{on=1, linger=0} so the next close() sends RST instead of FIN.
    // Used on every rejection path so clients fail fast (ECONNRESET) rather than
    // discovering capacity exhaustion only after a full TCP teardown.
    static void reject_with_rst(int fd) noexcept {
        linger sl{};
        sl.l_onoff  = 1;
        sl.l_linger = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
        // Caller's Socket RAII destructor calls close() -> RST dispatched by kernel
    }

    void enqueue_write(int fd, const char* data, ssize_t len) noexcept {
        if (fd < 0 || fd >= max_fds_) return;
        auto& buf = write_buffers_[fd];
        buf.insert(buf.end(), data, data + len);
        epoll_.modify_socket(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP);
    }

    void flush_write_buffer(int fd) noexcept {
        if (fd < 0 || fd >= max_fds_) return;
        auto& buf = write_buffers_[fd];

        size_t total_sent = 0;
        while (total_sent < buf.size()) {
            ssize_t res = ::write(fd, buf.data() + total_sent, buf.size() - total_sent);
            if (res < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                drop_connection(fd);
                return;
            }
            total_sent += static_cast<size_t>(res);
        }

        if (total_sent > 0) {
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(total_sent));
        }

        if (buf.empty()) {
            epoll_.modify_socket(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
        }
    }

    void handle_new_connection() noexcept {
        while (true) {
            auto client_opt = listen_sock_.accept_connection();
            if (!client_opt) break;

            int client_fd = client_opt->get_fd();

            // Circuit breaker: worker is at capacity — RST immediately so the client
            // gets ECONNRESET and can retry another node without a slow FIN teardown.
            if (active_connections_ >= max_connections_ || client_fd >= max_fds_) {
                reject_with_rst(client_fd);
                continue; // client_opt destructs -> close() -> RST
            }

            // RCU load: lock-free snapshot of the current backend list
            auto table = dns_resolver_->get_routing_table();
            if (!table || table->empty()) {
                reject_with_rst(client_fd);
                continue;
            }

            // Round-robin backend selection
            const sockaddr_in& dest = (*table)[round_robin_index_++ % table->size()];

            // Create a non-blocking outbound socket for the backend leg
            int backend_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (backend_fd < 0) {
                reject_with_rst(client_fd);
                continue;
            }
            if (backend_fd >= max_fds_) {
                ::close(backend_fd);
                reject_with_rst(client_fd);
                continue;
            }

            // Mirror the client-side TCP_NODELAY: disable Nagle on the backend leg
            // to prevent up to 40ms coalescing delay on small Protobuf payloads
            int nodelay = 1;
            ::setsockopt(backend_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            // Non-blocking connect: returns immediately with EINPROGRESS
            int ret = ::connect(backend_fd,
                                reinterpret_cast<const sockaddr*>(&dest),
                                sizeof(dest));
            if (ret < 0 && errno != EINPROGRESS) {
                ::close(backend_fd);
                reject_with_rst(client_fd);
                continue;
            }

            // Hand ownership to RAII socket wrapper
            client_sockets_[backend_fd] = Socket{FileDescriptor{backend_fd}};
            is_pending_connect_[backend_fd] = true;

            // Register backend: EPOLLOUT fires when the connect() completes
            if (!epoll_.add_socket(backend_fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP)) {
                client_sockets_[backend_fd].reset();
                is_pending_connect_[backend_fd] = false;
                reject_with_rst(client_fd);
                continue;
            }

            // Register client and link both ends as partners
            if (!epoll_.add_socket(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP)) {
                epoll_.remove_socket(backend_fd);
                client_sockets_[backend_fd].reset();
                is_pending_connect_[backend_fd] = false;
                reject_with_rst(client_fd);
                continue;
            }

            client_sockets_[client_fd] = std::move(*client_opt);
            socket_partners_[client_fd] = backend_fd;
            socket_partners_[backend_fd] = client_fd;
            ++active_connections_;
        }
    }

    void drop_connection(int fd) noexcept {
        if (fd < 0 || fd >= max_fds_) return;

        epoll_.remove_socket(fd);
        write_buffers_[fd].clear();
        is_pending_connect_[fd] = false;

        int partner_fd = socket_partners_[fd];
        if (partner_fd != -1 && partner_fd < max_fds_) {
            --active_connections_;
            epoll_.remove_socket(partner_fd);
            write_buffers_[partner_fd].clear();
            is_pending_connect_[partner_fd] = false;
            client_sockets_[partner_fd].reset();
            socket_partners_[partner_fd] = -1;
        }

        client_sockets_[fd].reset();
        socket_partners_[fd] = -1;
    }

public:
    // Each worker creates its own listen socket on the same port.
    // SO_REUSEPORT (set inside Socket::bind_and_listen) allows this.
    // The kernel distributes incoming connections across all workers.
    ProxyWorker(int worker_id, int port, std::shared_ptr<DnsResolver> dns,
                std::atomic<bool>& is_running, int max_fds)
        : worker_id_(worker_id),
          max_fds_(max_fds),
          epoll_(Epoll::create()),
          listen_sock_(Socket::create()),
          dns_resolver_(std::move(dns)),
          is_running_(is_running)
    {
        max_connections_ = max_fds_ / 2; // each pair consumes 2 fd slots

        client_sockets_.resize(max_fds_);
        socket_partners_.assign(max_fds_, -1);
        write_buffers_.resize(max_fds_);
        is_pending_connect_.assign(max_fds_, false);

        listen_sock_.bind_and_listen(port);
        epoll_.add_socket(listen_sock_.get_fd(), EPOLLIN | EPOLLET);
    }

    ProxyWorker(const ProxyWorker&) = delete;
    ProxyWorker& operator=(const ProxyWorker&) = delete;
    ProxyWorker(ProxyWorker&&) = delete;
    ProxyWorker& operator=(ProxyWorker&&) = delete;

    void run() noexcept {
        std::array<epoll_event, MAX_EVENTS> events;

        while (is_running_.load(std::memory_order_acquire)) {
            int num_events = epoll_.wait(events, 1000);
            if (num_events < 0) {
                if (errno == EINTR) continue;
                break;
            }

            for (int i = 0; i < num_events; ++i) {
                int active_fd = events[i].data.fd;
                uint32_t active_events = events[i].events;

                if (active_fd == listen_sock_.get_fd()) {
                    handle_new_connection();
                } else {
                    if (active_events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                        drop_connection(active_fd);
                        continue;
                    }

                    if (active_events & EPOLLIN) {
                        char buffer[4096];
                        while (true) {
                            ssize_t bytes_read = ::read(active_fd, buffer, sizeof(buffer));
                            if (bytes_read == 0) {
                                drop_connection(active_fd);
                                break;
                            } else if (bytes_read < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                                if (errno == EINTR) continue;
                                drop_connection(active_fd);
                                break;
                            }

                            if (active_fd >= max_fds_) break;
                            int partner_fd = socket_partners_[active_fd];
                            if (partner_fd != -1) {
                                if (!write_buffers_[partner_fd].empty()) {
                                    enqueue_write(partner_fd, buffer, bytes_read);
                                    continue;
                                }

                                ssize_t bytes_written = 0;
                                while (bytes_written < bytes_read) {
                                    ssize_t res = ::write(partner_fd, buffer + bytes_written, bytes_read - bytes_written);
                                    if (res < 0) {
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                            enqueue_write(partner_fd, buffer + bytes_written, bytes_read - bytes_written);
                                            break;
                                        }
                                        if (errno == EINTR) continue;
                                        drop_connection(partner_fd);
                                        break;
                                    }
                                    bytes_written += res;
                                }
                            }
                        }
                    }

                    if (active_events & EPOLLOUT) {
                        if (is_pending_connect_[active_fd]) {
                            int err = 0;
                            socklen_t err_len = sizeof(err);
                            ::getsockopt(active_fd, SOL_SOCKET, SO_ERROR, &err, &err_len);
                            if (err != 0) {
                                drop_connection(active_fd);
                                continue;
                            }
                            is_pending_connect_[active_fd] = false;
                        }
                        flush_write_buffer(active_fd);
                    }
                }
            }
        }
    }

    [[nodiscard]] int id() const noexcept { return worker_id_; }
};
