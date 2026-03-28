module;
#include <liburing.h>
#include <system_error>
#include <cerrno>
#include <stdexcept>

export module IoUring;

export class IoUring {
private:
    ::io_uring ring_;
    bool active_ = false;

public:
    static IoUring create(unsigned entries = 8192) {
        ::io_uring_params params{};
        
        params.flags = IORING_SETUP_SINGLE_ISSUER | 
                       IORING_SETUP_DEFER_TASKRUN | 
                       IORING_SETUP_COOP_TASKRUN |
                       IORING_SETUP_CQSIZE;
        params.cq_entries = 32768; 
        
        ::io_uring ring;
        int ret = ::io_uring_queue_init_params(entries, &ring, &params);
        if (ret < 0) {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params failed");
        }
        
        return IoUring{ring};
    }

    explicit IoUring(::io_uring ring) noexcept : ring_(ring), active_(true) {}

    ~IoUring() {
        if (active_) {
            ::io_uring_queue_exit(&ring_);
        }
    }

    IoUring(IoUring&& other) noexcept : ring_(other.ring_), active_(other.active_) {
        other.active_ = false;
    }

    IoUring& operator=(IoUring&& other) noexcept {
        if (this != &other) {
            if (active_) ::io_uring_queue_exit(&ring_);
            ring_ = other.ring_;
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;

    // --- State Access ---

    ::io_uring& get_ring() noexcept { return ring_; }

    io_uring_sqe* get_sqe() noexcept {
        return ::io_uring_get_sqe(&ring_);
    }

    // --- Submission API ---

    void submit() noexcept {
        ::io_uring_submit(&ring_);
    }

    int submit_and_wait(unsigned wait_nr) noexcept {
        return ::io_uring_submit_and_wait(&ring_, wait_nr);
    }

    // --- Completion API ---

    bool has_overflow() const noexcept {
        return ::io_uring_cq_has_overflow(&ring_);
    }

    void cq_advance(unsigned count) noexcept {
        ::io_uring_cq_advance(&ring_, count);
    }

    // --- Direct Descriptor Management ---

    void register_files_sparse(unsigned nr_files) {
        int ret = ::io_uring_register_files_sparse(&ring_, nr_files);
        if (ret < 0) {
            throw std::system_error(-ret, std::system_category(), "Failed to register sparse files");
        }
    }

    // Explicitly frees a slot in the fixed file table to prevent -ENFILE leaks.
    // Must be invoked during socket teardown or error states.
    void prep_close_direct(int direct_fd_index, void* user_data) noexcept {
        io_uring_sqe* sqe = get_sqe();
        if (sqe) {
            ::io_uring_prep_close_direct(sqe, direct_fd_index);
            ::io_uring_sqe_set_data(sqe, user_data);
        }
    }
};