module;
#include <cstdint>
#include <sys/socket.h>

export module OperationContext;

export enum class OpType : uint8_t {
    MULTISHOT_ACCEPT,
    BACKEND_SOCKET_CREATE,
    BACKEND_CONNECT_WAIT,
    CLIENT_READ,
    BACKEND_WRITE,
    CLOSE_CONNECTION
};

// 32-byte packed struct to ensure strict L1 cache alignment
export struct alignas(32) OperationContext {
    OpType type;
    int client_fd_direct;
    int backend_fd_direct;
    int tcp_nodelay_val = 1;

    ::linger linger_val = {1, 0};
    uint32_t active_ip_index = 0;
};
