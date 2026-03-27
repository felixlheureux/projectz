// The Global Module Fragment: used to safely include legacy C/C++ headers before the module begins
module;

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#include <utility>
#include <optional>

// Formally declare this file as the Socket Module
export module Socket;

import FileDescriptor;

export class Socket {
private:
    FileDescriptor fd_;

    // Private helper to set non-blocking flag. Returns false on failure (hot path compatible).
    static bool set_nonblocking_flag(int fd) noexcept {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            return false;
        }
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            return false;
        }
        return true;
    }

    // Disable Nagle's algorithm for low-latency telemetry forwarding
    static bool set_tcp_nodelay(int fd) noexcept {
        int opt = 1;
        return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
    }

public:
    // Factory method for creating non-blocking TCP sockets (Idiom 5)
    // Allowed to throw std::system_error since this is initial configuration phase (Idiom 3)
    [[nodiscard]] static Socket create(int domain = AF_INET, int type = SOCK_STREAM, int protocol = 0) {
        int raw_fd = ::socket(domain, type | SOCK_CLOEXEC, protocol);
        if (raw_fd < 0) {
            throw std::system_error(errno, std::system_category(), "Failed to create socket");
        }
        
        // Wrap descriptor immediately in RAII to prevent leaks on failure (Idiom 1)
        Socket sock{FileDescriptor{raw_fd}};
        
        // Enforce O_NONBLOCK immediately
        if (!set_nonblocking_flag(sock.get_fd())) {
            throw std::system_error(errno, std::system_category(), "Failed to set socket non-blocking");
        }
        
        return sock;
    }

    // Wrap an existing file descriptor, taking ownership
    explicit Socket(FileDescriptor&& fd) noexcept : fd_(std::move(fd)) {}

    // Move semantics required (Idiom 2)
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    
    // Explicitly delete copy semantics
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get_fd() const noexcept {
        return fd_.get();
    }

    // Configure and start listening (Configuration phase - Exceptions permitted)
    void bind_and_listen(int port, int backlog = 65535) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        int opt = 1;
        // SO_REUSEADDR and SO_REUSEPORT are distinct option names, NOT bitmask flags.
        // They must be set in separate setsockopt calls.
        if (::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
             throw std::system_error(errno, std::system_category(), "setsockopt SO_REUSEADDR failed");
        }
        if (::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
             throw std::system_error(errno, std::system_category(), "setsockopt SO_REUSEPORT failed");
        }

        if (::bind(fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::system_error(errno, std::system_category(), "bind failed");
        }

        if (::listen(fd_.get(), backlog) < 0) {
            throw std::system_error(errno, std::system_category(), "listen failed");
        }
    }

    // Accept incoming connection - Hot Path - No Exceptions (Idiom 3)
    // Returns std::nullopt if no connection is pending (EAGAIN / EWOULDBLOCK) or on error.
    // Uses accept4() to atomically set SOCK_NONBLOCK|SOCK_CLOEXEC, eliminating
    // the two-syscall window where the FD exists in blocking mode.
    std::optional<Socket> accept_connection() noexcept {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = ::accept4(fd_.get(), reinterpret_cast<sockaddr*>(&client_addr), 
                                  &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            return std::nullopt; // EAGAIN/EWOULDBLOCK are expected non-error states
        }

        // Disable Nagle for low-latency L4 forwarding
        set_tcp_nodelay(client_fd);
        
        Socket client_sock{FileDescriptor{client_fd}};
        return client_sock; // Return via move semantics
    }
};
