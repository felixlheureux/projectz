module;

#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <liburing.h>
#include <cerrno>
#include <sys/uio.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <cstring>

export module ProxyWorker;

import Socket;
import FileDescriptor;
import DnsResolver;
import IoUring;
import OperationContext;

export class ProxyWorker {
private:
    static constexpr unsigned RING_DEPTH = 32768;
    static constexpr int BUFFER_SIZE = 4096;
    static constexpr int BGID = 1;
    static constexpr int MAX_FDS = 1048576; // Expanded to 1 Million OS FDs

    int worker_id_;
    int port_;
    std::shared_ptr<DnsResolver> dns_resolver_;
    std::atomic<bool>& is_running_;
    
    Socket listen_sock_;
    IoUring ring_;

    std::vector<OperationContext> ctx_storage_;
    std::vector<OperationContext*> ctx_free_list_;

    size_t buffers_count_;
    ::io_uring_buf_ring *br_;
    void *br_ptr_;

    std::vector<bool> fd_is_open_;
    std::vector<int> pending_closes_;
    bool accept_queued_{false};

    OperationContext* acquire_ctx() {
        if (ctx_free_list_.empty()) return nullptr;
        OperationContext* ctx = ctx_free_list_.back();
        ctx_free_list_.pop_back();
        return ctx;
    }

    void release_ctx(OperationContext* ctx) {
        ctx_free_list_.push_back(ctx);
    }

    ::io_uring_sqe* get_sqe_safe() noexcept {
        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (!sqe) {
            ring_.submit();
            sqe = ring_.get_sqe();
        }
        return sqe;
    }

    sockaddr_in resolve_backend() {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(8081);
        dest.sin_addr.s_addr = inet_addr("127.0.0.1");
        return dest;
    }

    void submit_multishot_accept() noexcept {
        if (accept_queued_) return;

        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) return; 
        
        OperationContext* ctx = acquire_ctx();
        if (!ctx) return; 
        
        ctx->type = OpType::MULTISHOT_ACCEPT;
        
        // Use STANDARD multishot accept (no Fixed Files, bypasses io_wq limits)
        ::io_uring_prep_multishot_accept(sqe, listen_sock_.get_fd(), nullptr, nullptr, 0);
        ::io_uring_sqe_set_data(sqe, ctx);
        accept_queued_ = true;
    }

    void flush_pending_closes() noexcept {
        while (!pending_closes_.empty()) {
            ::io_uring_sqe* sqe = ring_.get_sqe();
            if (!sqe) {
                ring_.submit();
                sqe = ring_.get_sqe();
                if (!sqe) break; 
            }
            
            int fd = pending_closes_.back();
            pending_closes_.pop_back();
            // Use STANDARD close
            ::io_uring_prep_close(sqe, fd);
            ::io_uring_sqe_set_data(sqe, nullptr);
        }
    }

    void drop_fd(int fd) noexcept {
        if (fd < 0 || fd >= MAX_FDS || !fd_is_open_[fd]) return;
        fd_is_open_[fd] = false;
        pending_closes_.push_back(fd);
    }

    void submit_read_request(int fd, int target_fd) noexcept {
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) {
            drop_fd(fd);
            drop_fd(target_fd);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) {
            drop_fd(fd);
            drop_fd(target_fd);
            return;
        }

        ctx->type = OpType::CLIENT_READ;
        ctx->client_fd_direct = fd;
        ctx->backend_fd_direct = target_fd;

        ::io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT; // Removed IOSQE_FIXED_FILE
        sqe->buf_group = BGID;
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void submit_write_request(int target_fd, int source_fd, void* data, unsigned int len, unsigned short buffer_id) noexcept {
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) {
            drop_fd(target_fd);
            drop_fd(source_fd);
            ::io_uring_buf_ring_add(br_, data, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
            ::io_uring_buf_ring_advance(br_, 1);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) {
            drop_fd(target_fd);
            drop_fd(source_fd);
            ::io_uring_buf_ring_add(br_, data, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
            ::io_uring_buf_ring_advance(br_, 1);
            return;
        }

        ctx->type = OpType::BACKEND_WRITE;
        ctx->backend_fd_direct = target_fd; 
        ctx->client_fd_direct = source_fd;  
        ctx->active_ip_index = buffer_id;

        ::io_uring_prep_send(sqe, target_fd, data, len, 0);
        // Removed IOSQE_FIXED_FILE
        ::io_uring_sqe_set_data(sqe, ctx);
    }

public:
    ProxyWorker(int worker_id, int port, std::shared_ptr<DnsResolver> dns,
                std::atomic<bool>& is_running, int max_fds)
        : worker_id_(worker_id),
          port_(port),
          dns_resolver_(std::move(dns)),
          is_running_(is_running),
          listen_sock_(Socket::create()),
          ring_(IoUring::create(RING_DEPTH))
    {
        (void)max_fds;
        listen_sock_.bind_and_listen(port_);

        // Bypassing fixed file table limits completely
        fd_is_open_.assign(MAX_FDS, false);
        pending_closes_.reserve(131072);

        size_t pool_size = 131072;
        ctx_storage_.resize(pool_size);
        ctx_free_list_.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            ctx_free_list_.push_back(&ctx_storage_[i]);
        }

        buffers_count_ = 32768; 
        size_t br_size = buffers_count_ * BUFFER_SIZE;
        
        br_ptr_ = ::mmap(NULL, br_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (br_ptr_ == MAP_FAILED) {
            ::posix_memalign(&br_ptr_, 4096, br_size);
        }

        int ret = 0;
        br_ = ::io_uring_setup_buf_ring(&ring_.get_ring(), buffers_count_, BGID, 0, &ret);
        if (!br_) {
            std::cerr << "Failed to setup buf ring: " << ret << std::endl;
            exit(1);
        }

        ::io_uring_buf_ring_init(br_);
        char* base_ptr = static_cast<char*>(br_ptr_);
        for (unsigned short i = 0; i < buffers_count_; i++) {
            ::io_uring_buf_ring_add(br_, base_ptr + i * BUFFER_SIZE, BUFFER_SIZE, i+1, io_uring_buf_ring_mask(buffers_count_), i);
        }
        ::io_uring_buf_ring_advance(br_, buffers_count_);
    }

    [[nodiscard]] int id() const noexcept { return worker_id_; }

    void run() noexcept {
        std::cout << "[Loadbalancer] io_uring Proxy Loop Started (Worker " << worker_id_ << ")." << std::endl;

        submit_multishot_accept();

        while (is_running_.load(std::memory_order_acquire)) {
            flush_pending_closes();
            submit_multishot_accept(); 
            
            [[maybe_unused]] int ret = ring_.submit_and_wait(1);

            while (true) {
                constexpr unsigned MAX_CQE_BATCH = 4096;
                ::io_uring_cqe* cqes[MAX_CQE_BATCH];
                unsigned count = ::io_uring_peek_batch_cqe(&ring_.get_ring(), cqes, MAX_CQE_BATCH);
                if (count == 0) break;

                for (unsigned i = 0; i < count; ++i) {
                    ::io_uring_cqe* cqe = cqes[i];
                    auto* ctx = static_cast<OperationContext*>(::io_uring_cqe_get_data(cqe));
                    
                    if (!ctx) continue;

                    int res = cqe->res;
                    int flags = cqe->flags;

                    switch (ctx->type) {
                        case OpType::MULTISHOT_ACCEPT:
                            if (!(flags & IORING_CQE_F_MORE)) {
                                accept_queued_ = false;
                            }
                            if (res >= 0) {
                                int client_fd = res;
                                fd_is_open_[client_fd] = true;

                                // 1. Ultra-fast Synchronous Socket (bypasses io_wq bottleneck)
                                int backend_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                                if (backend_fd >= 0) {
                                    fd_is_open_[backend_fd] = true;

                                    // 2. Synchronous TIME_WAIT killer
                                    ::linger linger_val{1, 0};
                                    ::setsockopt(backend_fd, SOL_SOCKET, SO_LINGER, &linger_val, sizeof(linger_val));

                                    // 3. Synchronous Connect (nanosecond loopback routing)
                                    sockaddr_in dest = resolve_backend();
                                    ::connect(backend_fd, (sockaddr*)&dest, sizeof(dest));

                                    // 4. Instantly hit the io_uring data-plane!
                                    submit_read_request(client_fd, backend_fd);
                                    submit_read_request(backend_fd, client_fd);
                                } else {
                                    drop_fd(client_fd);
                                }
                            } else if (res < 0 && res != -EAGAIN && res != -EINTR && res != -ECANCELED) {
                                std::cerr << "[Worker " << worker_id_ << "] Accept Error: " << res << " (" << std::strerror(-res) << ")\n";
                            }
                            if (!(flags & IORING_CQE_F_MORE)) {
                                release_ctx(ctx);
                            }
                            break;

                        // Safely ignored to prevent -Wswitch compilation errors
                        case OpType::BACKEND_SOCKET_CREATE:
                        case OpType::BACKEND_SET_LINGER:
                        case OpType::BACKEND_CONNECT_WAIT:
                            release_ctx(ctx);
                            break;

                        case OpType::CLIENT_READ:
                            if (res > 0) {
                                unsigned short buffer_id = flags >> IORING_CQE_BUFFER_SHIFT;
                                char* buf_addr = static_cast<char*>(br_ptr_) + ((buffer_id - 1) * BUFFER_SIZE);
                                submit_write_request(ctx->backend_fd_direct, ctx->client_fd_direct, buf_addr, res, buffer_id);
                            } else if (res <= 0) {
                                drop_fd(ctx->client_fd_direct);
                                drop_fd(ctx->backend_fd_direct);
                            }
                            
                            if (!(flags & IORING_CQE_F_MORE)) {
                                release_ctx(ctx);
                            }
                            break;

                        case OpType::BACKEND_WRITE:
                            {
                                unsigned short buffer_id = ctx->active_ip_index;
                                char* buf_addr = static_cast<char*>(br_ptr_) + ((buffer_id - 1) * BUFFER_SIZE);
                                ::io_uring_buf_ring_add(br_, buf_addr, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
                                ::io_uring_buf_ring_advance(br_, 1);
                                
                                if (res < 0) {
                                    drop_fd(ctx->client_fd_direct);
                                    drop_fd(ctx->backend_fd_direct);
                                }
                                release_ctx(ctx);
                            }
                            break;

                        case OpType::CLOSE_CONNECTION:
                            release_ctx(ctx);
                            break;
                    }
                }
                ::io_uring_cq_advance(&ring_.get_ring(), count);
            }
        }
    }
};