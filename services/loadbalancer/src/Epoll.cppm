// The Global Module Fragment: used to safely include legacy C/C++ headers before the module begins
module;

#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#include <utility>
#include <span>

// Formally declare this file as the Epoll Module
export module Epoll;

import FileDescriptor;

export class Epoll {
private:
    FileDescriptor fd_;

public:
    // Factory method for creating the epoll instance.
    // Allowed to throw std::system_error since this is the initial configuration phase
    static Epoll create() {
        int epoll_fd = ::epoll_create1(0);
        if (epoll_fd < 0) {
            throw std::system_error(errno, std::system_category(), "Failed to create epoll instance");
        }
        // Use uniform initialization to prevent most vexing parse
        return Epoll{FileDescriptor{epoll_fd}};
    }

    // Wrap an existing file descriptor, taking ownership
    explicit Epoll(FileDescriptor&& fd) noexcept : fd_(std::move(fd)) {}

    // Enforce move semantics entirely
    Epoll(Epoll&&) noexcept = default;
    Epoll& operator=(Epoll&&) noexcept = default;
    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;

    [[nodiscard]] int get_fd() const noexcept {
        return fd_.get();
    }

    // Hot path methods. Error handling is deterministic (return bool).
    // The events parameter is mapped to epoll_event::events (e.g. EPOLLIN | EPOLLOUT | EPOLLET)
    bool add_socket(int socket_fd, uint32_t events) noexcept {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = socket_fd;
        
        return ::epoll_ctl(fd_.get(), EPOLL_CTL_ADD, socket_fd, &ev) == 0;
    }

    bool modify_socket(int socket_fd, uint32_t events) noexcept {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = socket_fd;
        
        return ::epoll_ctl(fd_.get(), EPOLL_CTL_MOD, socket_fd, &ev) == 0;
    }

    bool remove_socket(int socket_fd) noexcept {
        epoll_event ev{};
        return ::epoll_ctl(fd_.get(), EPOLL_CTL_DEL, socket_fd, &ev) == 0;
    }

    // Stack-allocated deterministic execution hot path (Idiom 4)
    // Takes a std::span for boundary safety without allocation overhead.
    // Returns number of events returned by epoll_wait.
    int wait(std::span<epoll_event> events_buffer, int timeout_ms = -1) noexcept {
        return ::epoll_wait(
            fd_.get(), 
            events_buffer.data(), 
            static_cast<int>(events_buffer.size()), 
            timeout_ms
        );
    }
};
