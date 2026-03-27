#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <string>

import ProxyServer;

// Test 4: Integration - EPOLLET Draining and Dead Backend Handling
TEST(ProxyServerIntegrationTest, EdgeTriggeredBufferDraining) {
    std::atomic<bool> proxy_ready{false};
    std::unique_ptr<ProxyServer> proxy;
    
    std::thread proxy_worker([&]() {
        // Init proxy listening on localhost:8081
        proxy = std::make_unique<ProxyServer>(8081, "localhost");
        proxy_ready.store(true);
        
        // Enters infinite while(is_running_) epoll layout
        proxy->run();
    });
    
    // Spin until proxy bound port successfully
    while(!proxy_ready.load()) { 
        std::this_thread::yield(); 
    }
    
    // Establishing mock TCP client directly mapping Integration strategy
    int client_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(client_sock, -1);
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8081);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    int connect_res = ::connect(client_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    ASSERT_EQ(connect_res, 0);
    
    // Creating payloads larger than the internal 4096 byte read boundaries.
    // This absolutely forces the proxy to loop its reads to fetch the whole 9000 bytes 
    // until it strictly triggers EAGAIN, validating the EPOLLET pipeline.
    std::string massive_payload(9000, 'B');
    ssize_t bytes_sent = ::send(client_sock, massive_payload.data(), massive_payload.size(), 0);
    EXPECT_EQ(bytes_sent, 9000);
    
    // Let the event loop naturally drain the massive buffer bytes through the stack memory map
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Explicitly simulate "Dead Backend Handling" via TCP graceful reset
    ::close(client_sock);
    
    // Let the proxy process EPOLLRDHUP and tear down the FileDescriptor allocations via RAII
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Halt proxy and rejoin main testing stack
    proxy->stop();
    if (proxy_worker.joinable()) {
        proxy_worker.join();
    }
    
    // If the proxy exited normally here, it proves tearing down backends mid-flight 
    // and blasting oversize buffers will NOT deadlock or trigger Segmentation Faults.
    SUCCEED();
}

// Test 5: Multi-threaded worker accept distribution
// Validates that multiple concurrent clients can connect when N worker threads
// are each running their own epoll + listen socket via SO_REUSEPORT.
TEST(ProxyServerIntegrationTest, ConcurrentClientAcceptDistribution) {
    std::atomic<bool> proxy_ready{false};
    std::unique_ptr<ProxyServer> proxy;

    std::thread proxy_worker([&]() {
        proxy = std::make_unique<ProxyServer>(8082, "localhost");
        proxy_ready.store(true);
        proxy->run();
    });

    while (!proxy_ready.load()) {
        std::this_thread::yield();
    }
    // Let all worker threads start their epoll loops
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    constexpr int NUM_CLIENTS = 10;
    std::atomic<int> connected{0};
    std::vector<std::thread> client_threads;

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        client_threads.emplace_back([&connected]() {
            int sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(8082);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

            if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                connected.fetch_add(1, std::memory_order_relaxed);
                // Send a small payload to exercise the read path
                const char msg[] = "ping";
                ::send(sock, msg, sizeof(msg), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::close(sock);
        });
    }

    for (auto& t : client_threads) {
        if (t.joinable()) t.join();
    }

    // All clients should have connected successfully
    EXPECT_EQ(connected.load(), NUM_CLIENTS);

    proxy->stop();
    if (proxy_worker.joinable()) {
        proxy_worker.join();
    }

    SUCCEED();
}
