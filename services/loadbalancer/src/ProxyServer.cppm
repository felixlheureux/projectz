// The Global Module Fragment
module;

#include <array>
#include <vector>
#include <string>
#include <optional>
#include <cerrno>
#include <unistd.h>
#include <sys/epoll.h>
#include <atomic>
#include <cstring>

export module ProxyServer;

import Socket;
import Epoll;
import DnsResolver;

export class ProxyServer {
private:
    static constexpr int MAX_EVENTS = 1024;
    static constexpr int MAX_FDS = 1000000; // Matches host OS tuning requirements from specs.md

    Epoll epoll_;
    Socket listen_sock_;
    DnsResolver dns_resolver_;
    std::atomic<bool> is_running_{true};

    // Deterministic Connection Pool (Idioms 1, 2, and 4)
    // Pre-allocated array indexed by the socket's integer file descriptor.
    // This provides O(1) lock-free lookup directly in the hot-path and
    // completely eliminates heap allocation fragmentation (`new`/`delete`).
    std::vector<std::optional<Socket>> client_sockets_;
    
    // Tracks bidirectional multiplexing pairings (client_fd -> backend_fd and vice versa)
    std::vector<int> socket_partners_;

    // Per-connection write buffers for back-pressure handling.
    // When write() returns EAGAIN, unwritten bytes are stored here and
    // EPOLLOUT is registered. When the socket becomes writable, the buffer
    // is drained and EPOLLOUT is removed once empty.
    std::vector<std::vector<char>> write_buffers_;

    void enqueue_write(int fd, const char* data, ssize_t len) noexcept {
        if (fd < 0 || fd >= MAX_FDS) return;
        auto& buf = write_buffers_[fd];
        buf.insert(buf.end(), data, data + len);
        // Register EPOLLOUT so we get notified when the socket is writable
        epoll_.modify_socket(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP);
    }

    void flush_write_buffer(int fd) noexcept {
        if (fd < 0 || fd >= MAX_FDS) return;
        auto& buf = write_buffers_[fd];
        
        size_t total_sent = 0;
        while (total_sent < buf.size()) {
            ssize_t res = ::write(fd, buf.data() + total_sent, buf.size() - total_sent);
            if (res < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                // Fatal write error on this FD
                drop_connection(fd);
                return;
            }
            total_sent += static_cast<size_t>(res);
        }

        if (total_sent > 0) {
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(total_sent));
        }

        // Buffer fully drained: remove EPOLLOUT to stop writable notifications
        if (buf.empty()) {
            epoll_.modify_socket(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
        }
    }

    void handle_new_connection() noexcept {
        while (true) {
            auto client_opt = listen_sock_.accept_connection();
            if (!client_opt) {
                // EAGAIN or EWOULDBLOCK reached: we've successfully drained the accept queue edge.
                break; 
            }

            int client_fd = client_opt->get_fd();
            if (client_fd >= MAX_FDS) {
                // To safely drop it, simply let `client_opt` go out of scope, RAII will cleanly `close()` it.
                continue; 
            }

            // Register for EPOLLIN with Edge Trigger (EPOLLET) and EPOLLRDHUP for clean peer shutdown detection.
            // EPOLLOUT is intentionally NOT registered here to avoid thundering herd on idle connections.
            // It is added dynamically by enqueue_write() only when there is pending write data.
            if (epoll_.add_socket(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP)) {
                client_sockets_[client_fd] = std::move(*client_opt);
                socket_partners_[client_fd] = -1;
            }
        }
    }

    void drop_connection(int fd) noexcept {
        // Bounds check to prevent out-of-bounds vector access on unexpected FDs
        if (fd < 0 || fd >= MAX_FDS) return;

        epoll_.remove_socket(fd);
        write_buffers_[fd].clear();
        
        int partner_fd = socket_partners_[fd];
        if (partner_fd != -1 && partner_fd < MAX_FDS) {
            epoll_.remove_socket(partner_fd);
            write_buffers_[partner_fd].clear();
            client_sockets_[partner_fd].reset();
            socket_partners_[partner_fd] = -1;
        }

        client_sockets_[fd].reset();
        socket_partners_[fd] = -1;
    }

public:
    // Initialization Phase - Can throw on failure (Idiom 3)
    ProxyServer(int port, std::string backend_hostname)
        : epoll_(Epoll::create()),
          listen_sock_(Socket::create()),
          dns_resolver_(std::move(backend_hostname), 8080)
    {
        // Pre-allocate the deterministic routing/connection space
        client_sockets_.resize(MAX_FDS);
        socket_partners_.assign(MAX_FDS, -1);
        write_buffers_.resize(MAX_FDS);

        listen_sock_.bind_and_listen(port);
        epoll_.add_socket(listen_sock_.get_fd(), EPOLLIN | EPOLLET);
    }

    void stop() noexcept {
        is_running_.store(false, std::memory_order_release);
    }

    // Run the Proxy - Exception-Free Hot Path (Idiom 3)
    void run() noexcept {
        // Stack-Allocated Contiguous Memory Buffer (Idiom 4)
        std::array<epoll_event, MAX_EVENTS> events;

        while (is_running_.load(std::memory_order_acquire)) {
            // Unblocks periodically to check atomic stop flag instead of hanging infinitely
            int num_events = epoll_.wait(events, 1000);
            if (num_events < 0) {
                if (errno == EINTR) continue;
                break; // A fatal underlying system error terminated the poll loop
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
                            
                            if (active_fd >= MAX_FDS) break;
                            int partner_fd = socket_partners_[active_fd];
                            if (partner_fd != -1) {
                                // If backlog already exists, append directly—no point attempting a write
                                // that will just pile up out-of-order data.
                                if (!write_buffers_[partner_fd].empty()) {
                                    enqueue_write(partner_fd, buffer, bytes_read);
                                    continue;
                                }

                                ssize_t bytes_written = 0;
                                while (bytes_written < bytes_read) {
                                    ssize_t res = ::write(partner_fd, buffer + bytes_written, bytes_read - bytes_written);
                                    if (res < 0) {
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                            // Buffer the remaining unsent bytes and register EPOLLOUT
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
                        flush_write_buffer(active_fd);
                    }
                }
            }
        }
    }
};
