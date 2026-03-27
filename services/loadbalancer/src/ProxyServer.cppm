// The Global Module Fragment
module;

#include <iostream>
#include <array>
#include <vector>
#include <optional>
#include <cerrno>
#include <unistd.h>
#include <sys/epoll.h>
#include <memory>

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

    // Deterministic Connection Pool (Idioms 1, 2, and 4)
    // Pre-allocated array indexed by the socket's integer file descriptor.
    // This provides O(1) lock-free lookup directly in the hot-path and
    // completely eliminates heap allocation fragmentation (`new`/`delete`).
    std::vector<std::optional<Socket>> client_sockets_;
    
    // Tracks bidirectional multiplexing pairings (client_fd -> backend_fd and vice versa)
    // For simplicity of this demonstration, we just track partner FDs as basic ints.
    std::vector<int> socket_partners_;

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

            // Bidirectional Multiplexing (Specs.md): Register for EPOLLIN and EPOLLOUT with Edge Trigger (EPOLLET)
            if (epoll_.add_socket(client_fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
                // Strict Move Semantics (Idiom 2): Transfer ownership of the Socket into our pre-allocated slab.
                client_sockets_[client_fd] = std::move(*client_opt);
                socket_partners_[client_fd] = -1; // Unpaired initially until backend routing logic executes
            }
        }
    }

    void drop_connection(int fd) noexcept {
        epoll_.remove_socket(fd);
        
        int partner_fd = socket_partners_[fd];
        if (partner_fd != -1) {
            epoll_.remove_socket(partner_fd);
            client_sockets_[partner_fd].reset(); // Destroy RAII socket, triggering close()
            socket_partners_[partner_fd] = -1;
        }

        client_sockets_[fd].reset(); // Destroy RAII socket, triggering close() and preventing leak (Idiom 1)
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

        listen_sock_.bind_and_listen(port);
        epoll_.add_socket(listen_sock_.get_fd(), EPOLLIN | EPOLLET);
    }

    // Run the Proxy - Exception-Free Hot Path (Idiom 3)
    void run() noexcept {
        // Stack-Allocated Contiguous Memory Buffer (Idiom 4)
        std::array<epoll_event, MAX_EVENTS> events;

        std::cout << "[Loadbalancer] Proxy Loop Started. Awaiting telemetry..." << std::endl;

        while (true) {
            int num_events = epoll_.wait(events, -1);
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
                    // Dropped / Closed connections handling
                    if (active_events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                        drop_connection(active_fd);
                        continue;
                    }

                    // Bidirectional Multiplexing (Specs.md Component Deep Dive)
                    if (active_events & EPOLLIN) {
                        // Received Ingress Telemetry from the Edge Agent OR an explicit Application ACK from Go Ingestion Pod
                        // Buffer -> Route via DnsResolver -> Forward to Partner FD
                    }
                    if (active_events & EPOLLOUT) {
                        // Socket is writable: Drain our local application proxy ring-buffer to the TCP network buffer
                    }
                }
            }
        }
    }
};
