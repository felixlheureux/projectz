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

export module ProxyWorker;

import Socket;
import FileDescriptor;
import DnsResolver;
import IoUring;
import OperationContext;

export class ProxyWorker {
private:
    static constexpr unsigned SQ_DEPTH = 8192;
    static constexpr unsigned CQ_DEPTH = 32768;
    static constexpr int BUFFER_SIZE = 4096;
    static constexpr int BGID = 1;

    int worker_id_;
    int port_;
    std::shared_ptr<DnsResolver> dns_resolver_;
    std::atomic<bool>& is_running_;
    
    Socket listen_sock_;
    IoUring ring_;

    // Object pool for contexts
    std::vector<OperationContext> ctx_storage_;
    std::vector<OperationContext*> ctx_free_list_;

    int ring_fd_;
    size_t buffers_count_;
    struct io_uring_buf_ring *br_;
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
        auto table = dns_resolver_->get_routing_table();
        if (!table || table->empty()) return sockaddr_in{};
        
        return (*table)[round_robin_index_++ % table->size()];
    }

    void submit_multishot_accept() noexcept {
        ::io_uring_sqe* sqe = ring_.get_sqe();
        if (!sqe) return;
        
        OperationContext* ctx = acquire_ctx();
        if (!ctx) return;
        
        ctx->type = OpType::MULTISHOT_ACCEPT;
        
        ::io_uring_prep_multishot_accept_direct(sqe, listen_sock_.get_fd(), nullptr, nullptr, 0);
        ::io_uring_sqe_set_data(sqe, ctx);
    }

    void queue_backend_connect_chain(int client_direct_fd) noexcept {
        OperationContext* ctx = acquire_ctx();
        if (!ctx) return;
        ctx->type = OpType::BACKEND_CONNECT_CHAIN;
        ctx->client_fd_direct = client_direct_fd;

        // Step 1: Instantiate Backend Socket Asynchronously
        ::io_uring_sqe* sqe1 = ring_.get_sqe();
        ::io_uring_prep_socket_direct(sqe1, AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, IORING_FILE_INDEX_ALLOC, 0);
        sqe1->flags |= IOSQE_IO_LINK;
        ::io_uring_sqe_set_data(sqe1, ctx);

        // Step 2: Configure TCP_NODELAY Asynchronously
        ::io_uring_sqe* sqe2 = ring_.get_sqe();
        // fd 0, wait, io_uring link fd is implicitly previous when using SQE link ? No, we must use file_index if we know it?
        // Wait! In linux, linked operations don't automatically use the allocated descriptor from the previous IORING_OP_SOCKET!
        // Actually, they added support so we can't easily reference an unknown descriptor... wait, we CAN if we pre-allocate an exact file index!
        // Instead of IORING_FILE_INDEX_ALLOC, we allocate from our sparse table logically?
        // Wait, if IORING_OP_SOCKET returns the fd in CQE, we cannot link SETSOCKOPT because we don't know the fd!
        // Let's manually manage a free list of direct descriptor indices (from 1 to 65535)!
        // For now, let me use the standard IORING_OP_CONNECT with connect...
