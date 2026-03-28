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
#include <sys/mman.h>
#include <arpa/inet.h>

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

    size_t round_robin_index_{0};

    OperationContext* acquire_ctx() {
        if (ctx_free_list_.empty()) return nullptr;
        OperationContext* ctx = ctx_free_list_.back();
        ctx_free_list_.pop_back();
        return ctx;
    }

    void release_ctx(OperationContext* ctx) {
        ctx_free_list_.push_back(ctx);
    }

    sockaddr_in resolve_backend() {
        // Source/Dest IP Multiplexing to expand 4-tuple space and prevent exhaustion
        // Generates 127.0.0.1 through 127.0.0.20 as the destination IP
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(8081);
        
        uint32_t base_ip = ntohl(inet_addr("127.0.0.1"));
        uint32_t ip_offset = (round_robin_index_++ % 20);
        dest.sin_addr.s_addr = htonl(base_ip + ip_offset);
        
        return dest;
    }

    void submit_multishot_accept() noexcept {
        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (!sqe) return;
        
        OperationContext* ctx = acquire_ctx();
        if (!ctx) return;
        
        ctx->type = OpType::MULTISHOT_ACCEPT;
        
        // Use direct alloc for zero-overhead socket mapping
        ::io_uring_prep_multishot_accept_direct(sqe, listen_sock_.get_fd(), nullptr, nullptr, 0);
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    // Step 1: Allocate Backend Socket
    void queue_backend_socket(int client_direct_fd) noexcept {
        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (!sqe) {
            drop_connection(client_direct_fd, -1);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) { drop_connection(client_direct_fd, -1); return; }

        ctx->type = OpType::BACKEND_SOCKET_CREATE;
        ctx->client_fd_direct = client_direct_fd;
        ctx->backend_fd_direct = -1;

        // Step 1: Allocate Backend Socket
        ::io_uring_prep_socket_direct(sqe, AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, IORING_FILE_INDEX_ALLOC, 0);
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    // Step 2: Establish the backend TCP connection
    void queue_backend_connect(OperationContext* ctx, const sockaddr_in& dest_addr) noexcept {
        ::io_uring_sqe* sqe_conn = ring_.get_sqe();
        if (!sqe_conn) {
            drop_connection(ctx->client_fd_direct, ctx->backend_fd_direct);
            return;
        }

        ctx->type = OpType::BACKEND_CONNECT_WAIT;

        // Execute Connect
        ::io_uring_prep_connect(sqe_conn, ctx->backend_fd_direct, (sockaddr*)&dest_addr, sizeof(dest_addr));
        sqe_conn->flags |= IOSQE_FIXED_FILE;
        ::io_uring_sqe_set_data(sqe_conn, ctx);
    }

    void submit_read_request(int fd_direct, int target_fd_direct) noexcept {
        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (!sqe) return;

        OperationContext* ctx = acquire_ctx();
        if (!ctx) {
            drop_connection(fd_direct, target_fd_direct);
            return;
        }

        ctx->type = OpType::CLIENT_READ;
        ctx->client_fd_direct = fd_direct;
        ctx->backend_fd_direct = target_fd_direct;

        ::io_uring_prep_recv_multishot(sqe, fd_direct, nullptr, 0, 0);
        sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        sqe->buf_group = BGID;
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void submit_write_request(int target_fd_direct, int source_fd_direct, void* data, unsigned int len, unsigned short buffer_id) noexcept {
        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (!sqe) {
            drop_connection(target_fd_direct, source_fd_direct);
            ::io_uring_buf_ring_add(br_, data, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
            ::io_uring_buf_ring_advance(br_, 1);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) {
            drop_connection(target_fd_direct, source_fd_direct);
            ::io_uring_buf_ring_add(br_, data, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
            ::io_uring_buf_ring_advance(br_, 1);
            return;
        }

        ctx->type = OpType::BACKEND_WRITE;
        ctx->backend_fd_direct = target_fd_direct; 
        ctx->client_fd_direct = source_fd_direct;  
        ctx->active_ip_index = buffer_id; // temporarily store buffer ID

        ::io_uring_prep_send(sqe, target_fd_direct, data, len, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void drop_connection(int fd1_direct, int fd2_direct) noexcept {
        auto close_fd = [this](int fd_direct) {
            if (fd_direct < 0) return;
            ::io_uring_sqe* sqe_close = ring_.get_sqe();
            
            if (!sqe_close) {
                // If the SQ ring is completely full, we must submit to flush it, 
                // then grab a new SQE to ensure the close command is forced through.
                ring_.submit(); 
                sqe_close = ring_.get_sqe();
                if (!sqe_close) return; // Extreme edge case
            }
            
            ::io_uring_prep_close_direct(sqe_close, fd_direct);
            
            // We don't need to track the Completion Queue Entry for a close.
            // Pass nullptr so we don't consume our ctx_free_list_ pool!
            ::io_uring_sqe_set_data(sqe_close, nullptr); 
        };

        if (fd1_direct >= 0) close_fd(fd1_direct);
        if (fd2_direct >= 0) close_fd(fd2_direct);
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

        // Register direct descriptor sparse table
        ring_.register_files_sparse(65536);

        size_t pool_size = 131072;
        ctx_storage_.resize(pool_size);
        ctx_free_list_.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            ctx_free_list_.push_back(&ctx_storage_[i]);
        }

        buffers_count_ = 32768; // Power of 2 required
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
            // submit and wait for 1 event cooperatively
            int ret = ring_.submit_and_wait(1);
            if (ret < 0 && ret != -EBUSY && ret != -EINTR && ret != -ETIME) {
                // If overflow or fatal error, try to reap anyway
            }

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
                            if (res >= 0) {
                                queue_backend_socket(res);
                            }
                            // If IORING_CQE_F_MORE is clear, the multishot accept was terminated
                            if (!(flags & IORING_CQE_F_MORE)) {
                                release_ctx(ctx);
                                if (is_running_.load(std::memory_order_relaxed)) {
                                    submit_multishot_accept();
                                }
                            }
                            break;

                        case OpType::BACKEND_SOCKET_CREATE:
                            if (res >= 0) {
                                ctx->backend_fd_direct = res; // Save direct descriptor
                                sockaddr_in dest = resolve_backend();
                                queue_backend_connect(ctx, dest);
                            } else {
                                drop_connection(ctx->client_fd_direct, ctx->backend_fd_direct);
                                release_ctx(ctx); // [FIX] Release on error
                            }
                            break;

                        case OpType::BACKEND_CONNECT_WAIT:
                            if (res == 0) {
                                // Phase 2: Connect completed successfully
                                submit_read_request(ctx->client_fd_direct, ctx->backend_fd_direct);
                                submit_read_request(ctx->backend_fd_direct, ctx->client_fd_direct);
                            } else {
                                drop_connection(ctx->client_fd_direct, ctx->backend_fd_direct);
                            }
                            release_ctx(ctx); // [FIX] The connect phase is over. Free the context.
                            break;

                        case OpType::CLIENT_READ:
                            if (res > 0) {
                                unsigned short buffer_id = flags >> IORING_CQE_BUFFER_SHIFT;
                                char* buf_addr = static_cast<char*>(br_ptr_) + ((buffer_id - 1) * BUFFER_SIZE);
                                // Send data to destination
                                submit_write_request(ctx->backend_fd_direct, ctx->client_fd_direct, buf_addr, res, buffer_id);
                            } else if (res <= 0) {
                                drop_connection(ctx->client_fd_direct, ctx->backend_fd_direct);
                            }
                            
                            if (!(flags & IORING_CQE_F_MORE)) {
                                release_ctx(ctx);
                            }
                            break;

                        case OpType::BACKEND_WRITE:
                            {
                                unsigned short buffer_id = ctx->active_ip_index;
                                char* buf_addr = static_cast<char*>(br_ptr_) + ((buffer_id - 1) * BUFFER_SIZE);
                                // Restore buffer to ring
                                ::io_uring_buf_ring_add(br_, buf_addr, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
                                ::io_uring_buf_ring_advance(br_, 1);
                                
                                if (res < 0) {
                                    drop_connection(ctx->client_fd_direct, ctx->backend_fd_direct);
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
