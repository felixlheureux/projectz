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

// SOCKET_URING_OP_GETSOCKOPT/SETSOCKOPT were added in kernel 6.7 / liburing 2.6.
// The installed liburing-dev 2.5 only defines SIOCINQ=0 and SIOCOUTQ=1.
// The kernel ABI value is stable, so we define it here for older header sets.
#ifndef SOCKET_URING_OP_SETSOCKOPT
#define SOCKET_URING_OP_SETSOCKOPT 3
#endif
#include <sys/uio.h>
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
    static constexpr int MAX_FDS = 65536;

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

    // [FIX 1] Track explicit state to prevent Double-Free socket corruption
    std::vector<bool> fd_is_open_;
    // [FIX 2] Buffer closes if the kernel ring is temporarily full
    std::vector<int> pending_closes_;

    OperationContext* acquire_ctx() {
        if (ctx_free_list_.empty()) return nullptr;
        OperationContext* ctx = ctx_free_list_.back();
        ctx_free_list_.pop_back();
        return ctx;
    }

    void release_ctx(OperationContext* ctx) {
        ctx_free_list_.push_back(ctx);
    }

    void flush_pending_closes() noexcept {
        while (!pending_closes_.empty()) {
            ::io_uring_sqe* sqe = ring_.get_sqe();
            if (!sqe) break; // Ring still full, try again next loop
            
            int fd = pending_closes_.back();
            pending_closes_.pop_back();
            ::io_uring_prep_close_direct(sqe, fd);
            ::io_uring_sqe_set_data(sqe, nullptr);
        }
    }

    ::io_uring_sqe* get_sqe_safe() noexcept {
        flush_pending_closes();
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
        
        uint32_t base_ip = ntohl(inet_addr("127.0.0.1"));
        uint32_t ip_offset = (round_robin_index_++ % 20);
        dest.sin_addr.s_addr = htonl(base_ip + ip_offset);
        
        return dest;
    }

    void submit_multishot_accept() noexcept {
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) return;
        
        OperationContext* ctx = acquire_ctx();
        if (!ctx) return;
        
        ctx->type = OpType::MULTISHOT_ACCEPT;
        
        ::io_uring_prep_multishot_accept_direct(sqe, listen_sock_.get_fd(), nullptr, nullptr, 0);
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void queue_backend_socket(int client_direct_fd) noexcept {
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) {
            drop_fd(client_direct_fd);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) { drop_fd(client_direct_fd); return; }

        ctx->type = OpType::BACKEND_SOCKET_CREATE;
        ctx->client_fd_direct = client_direct_fd;
        ctx->backend_fd_direct = -1;

        ::io_uring_prep_socket_direct(sqe, AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, IORING_FILE_INDEX_ALLOC, 0);
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void queue_backend_linger(OperationContext* ctx) noexcept {
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) {
            drop_fd(ctx->client_fd_direct);
            drop_fd(ctx->backend_fd_direct);
            release_ctx(ctx);
            return;
        }

        ctx->type = OpType::BACKEND_SET_LINGER;

        // Set SO_LINGER{1,0} on the backend socket via IORING_OP_URING_CMD so that
        // close() sends RST instead of FIN, skipping the TIME_WAIT state entirely.
        // io_uring_prep_cmd_sock() in liburing 2.5 is a stub that ignores the
        // level/optname/optval/optlen args; patch them manually using the stable
        // kernel UAPI field layout (Linux 6.7+, present in linux/io_uring.h).
        ::io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT,
                                  ctx->backend_fd_direct, 0, 0, nullptr, 0);
        sqe->flags |= IOSQE_FIXED_FILE;

        // level (SOL_SOCKET=1) and optname (SO_LINGER=13) share sqe->addr as two
        // consecutive __u32s (lower = level, upper = optname).
        sqe->addr = static_cast<uint64_t>(SOL_SOCKET) |
                    (static_cast<uint64_t>(SO_LINGER) << 32);

        // optval pointer → sqe->cmd[0..7] (aliases sqe->addr3 at SQE offset 48).
        *reinterpret_cast<uint64_t*>(&sqe->cmd[0]) =
            reinterpret_cast<uint64_t>(&ctx->linger_val);

        // optlen → sqe->cmd[8..11] (SQE offset 56).  The kernel reads optlen from
        // this exact location for SOCKET_URING_OP_SETSOCKOPT.  Previously this was
        // incorrectly written to sqe->file_index (offset 44), causing the kernel
        // to see optlen=0 and silently reject the setsockopt.
        *reinterpret_cast<uint32_t*>(&sqe->cmd[8]) =
            static_cast<uint32_t>(sizeof(::linger));

        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void queue_backend_connect(OperationContext* ctx, const sockaddr_in& dest_addr) noexcept {
        ::io_uring_sqe* sqe_conn = get_sqe_safe();
        if (!sqe_conn) {
            drop_fd(ctx->client_fd_direct);
            drop_fd(ctx->backend_fd_direct);
            release_ctx(ctx);
            return;
        }

        ctx->type = OpType::BACKEND_CONNECT_WAIT;

        ::io_uring_prep_connect(sqe_conn, ctx->backend_fd_direct, (sockaddr*)&dest_addr, sizeof(dest_addr));
        sqe_conn->flags |= IOSQE_FIXED_FILE;
        ::io_uring_sqe_set_data(sqe_conn, ctx);
    }

    void submit_read_request(int fd_direct, int target_fd_direct) noexcept {
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) {
            drop_fd(fd_direct);
            drop_fd(target_fd_direct);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) {
            drop_fd(fd_direct);
            drop_fd(target_fd_direct);
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
        ::io_uring_sqe* sqe = get_sqe_safe();
        if (!sqe) {
            drop_fd(target_fd_direct);
            drop_fd(source_fd_direct);
            ::io_uring_buf_ring_add(br_, data, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
            ::io_uring_buf_ring_advance(br_, 1);
            return;
        }

        OperationContext* ctx = acquire_ctx();
        if (!ctx) {
            drop_fd(target_fd_direct);
            drop_fd(source_fd_direct);
            ::io_uring_buf_ring_add(br_, data, BUFFER_SIZE, buffer_id, io_uring_buf_ring_mask(buffers_count_), buffer_id - 1);
            ::io_uring_buf_ring_advance(br_, 1);
            return;
        }

        ctx->type = OpType::BACKEND_WRITE;
        ctx->backend_fd_direct = target_fd_direct; 
        ctx->client_fd_direct = source_fd_direct;  
        ctx->active_ip_index = buffer_id;

        ::io_uring_prep_send(sqe, target_fd_direct, data, len, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    // [FIX 3] Atomic state tracking ensures no Double-Frees and NO LEAKS
    void drop_fd(int fd_direct) noexcept {
        if (fd_direct < 0 || fd_direct >= MAX_FDS || !fd_is_open_[fd_direct]) return;
        
        fd_is_open_[fd_direct] = false;

        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (sqe) {
            ::io_uring_prep_close_direct(sqe, fd_direct);
            ::io_uring_sqe_set_data(sqe, nullptr);
        } else {
            // Guarantee resource teardown even during extreme ring congestion
            pending_closes_.push_back(fd_direct);
        }
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

        ring_.register_files_sparse(MAX_FDS);
        fd_is_open_.assign(MAX_FDS, false);
        pending_closes_.reserve(MAX_FDS);

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
                            if (res >= 0) {
                                fd_is_open_[res] = true; // [FIX 1] Claim the FD
                                queue_backend_socket(res);
                            }
                            if (!(flags & IORING_CQE_F_MORE)) {
                                release_ctx(ctx);
                                if (is_running_.load(std::memory_order_relaxed)) {
                                    submit_multishot_accept();
                                }
                            }
                            break;

                        case OpType::BACKEND_SOCKET_CREATE:
                            if (res >= 0) {
                                fd_is_open_[res] = true; // [FIX 1] Claim the FD
                                ctx->backend_fd_direct = res;
                                // Chain SO_LINGER{1,0} before connecting so the
                                // kernel skips TIME_WAIT on close, preventing
                                // nf_conntrack/tcp_max_tw_buckets exhaustion at 262k.
                                queue_backend_linger(ctx);
                            } else {
                                drop_fd(ctx->client_fd_direct);
                                release_ctx(ctx);
                            }
                            break;

                        case OpType::BACKEND_SET_LINGER:
                            {
                                // If the async setsockopt failed, the socket will
                                // close with FIN (TIME_WAIT) instead of RST.  This
                                // is non-fatal — proceed with connect regardless so
                                // the proxy keeps functioning, just less efficiently.
                                if (res < 0) {
                                    std::cerr << "[Worker " << worker_id_
                                              << "] BACKEND_SET_LINGER failed: "
                                              << res << std::endl;
                                }
                                sockaddr_in dest = resolve_backend();
                                queue_backend_connect(ctx, dest);
                            }
                            break;

                        case OpType::BACKEND_CONNECT_WAIT:
                            if (res == 0) {
                                submit_read_request(ctx->client_fd_direct, ctx->backend_fd_direct);
                                submit_read_request(ctx->backend_fd_direct, ctx->client_fd_direct);
                            } else {
                                drop_fd(ctx->client_fd_direct);
                                drop_fd(ctx->backend_fd_direct);
                            }
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