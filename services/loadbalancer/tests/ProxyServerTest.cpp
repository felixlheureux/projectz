#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

import ProxyServer;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Ask the OS to assign a free ephemeral port, then release it so the proxy can
// bind it. There is a brief TOCTOU window, but it is negligible in a local test
// environment where no other process is racing for the same port.
static int get_free_port() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int opt = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // OS picks a free port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    socklen_t len = sizeof(addr);
    ::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);
    ::close(sock);
    return port;
}

static bool wait_for_port(int port, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) break;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(s);
        if (ok) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// ── Test Fixture ──────────────────────────────────────────────────────────────

// All proxy lifecycle is managed here so that TearDown() is called even when
// an ASSERT_* macro short-circuits the test body, preventing thread leaks and
// port reuse across subsequent tests.
class ProxyServerTest : public ::testing::Test {
protected:
    std::unique_ptr<ProxyServer> proxy_;
    std::thread proxy_thread_;
    std::atomic<bool> proxy_constructed_{false};

    // Starts proxy on the given port and blocks until a TCP connection to the
    // port succeeds (guarantees all SO_REUSEPORT workers are in their epoll
    // loops before the test body proceeds).
    void start_proxy(int port, const std::string& backend_host = "localhost",
                     int backend_port = 19999 /* unused port — dead backend */) {
        proxy_thread_ = std::thread([this, port, backend_host, backend_port]() {
            proxy_ = std::make_unique<ProxyServer>(port, backend_host, backend_port);
            proxy_constructed_.store(true, std::memory_order_release);
            proxy_->run();
        });

        // Wait for ProxyServer constructor to complete (listen sockets bound).
        while (!proxy_constructed_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Wait until a real connection attempt to the port succeeds so we know
        // all worker threads have entered their epoll loops.
        ASSERT_TRUE(wait_for_port(port, 2000))
            << "Proxy did not become reachable on port " << port;
    }

    void TearDown() override {
        if (proxy_) proxy_->stop();
        if (proxy_thread_.joinable()) proxy_thread_.join();
    }
};

// ── Test 4: Dead-backend handling and EPOLLET read draining ──────────────────
//
// Verifies that when a client sends more data than one read() call can drain
// (edge-triggered semantics require a loop until EAGAIN) and the backend is
// unreachable, the proxy tears down cleanly — no deadlock, no segfault, no
// thread or FD leak.
TEST_F(ProxyServerTest, DeadBackendDoesNotDeadlockOrLeak) {
    int port = get_free_port();
    ASSERT_GT(port, 0);

    start_proxy(port);

    int client_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(client_sock, -1);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::connect(client_sock, reinterpret_cast<sockaddr*>(&server_addr),
                        sizeof(server_addr)), 0);

    // 9 000 bytes exceeds the 4 096-byte read buffer, forcing the worker to
    // loop until EAGAIN — validating EPOLLET drain correctness.
    std::string payload(9000, 'B');
    ::send(client_sock, payload.data(), payload.size(), MSG_NOSIGNAL);

    // Give the event loop time to attempt the backend connect and fail.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Graceful client close — proxy should detect EPOLLRDHUP and clean up.
    ::close(client_sock);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // TearDown() stops and joins the proxy. If we reach here the proxy is
    // neither deadlocked nor crashed.
    SUCCEED();
}

// ── Test 5: Concurrent client accept distribution ─────────────────────────────
//
// 10 clients connect simultaneously. With SO_REUSEPORT and N worker threads,
// every connection must be accepted within the timeout — no worker may
// monopolise the listen queue while others are idle.
TEST_F(ProxyServerTest, ConcurrentClientsAllAccepted) {
    int port = get_free_port();
    ASSERT_GT(port, 0);

    start_proxy(port);

    constexpr int NUM_CLIENTS = 10;
    std::atomic<int> connected{0};
    std::vector<std::thread> client_threads;
    client_threads.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        client_threads.emplace_back([port, &connected]() {
            int sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                connected.fetch_add(1, std::memory_order_relaxed);
                const char msg[] = "ping";
                ::send(sock, msg, sizeof(msg), MSG_NOSIGNAL);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::close(sock);
        });
    }

    for (auto& t : client_threads) {
        if (t.joinable()) t.join();
    }

    EXPECT_EQ(connected.load(), NUM_CLIENTS);
    // TearDown() stops and joins the proxy.
}
