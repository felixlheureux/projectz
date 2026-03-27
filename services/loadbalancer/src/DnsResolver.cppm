// The Global Module Fragment: used to safely include legacy C/C++ headers before the module begins
module;

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <netdb.h>
#include <arpa/inet.h>
#include <cstring>

// Formally declare this file as the DnsResolver Module
export module DnsResolver;

export class DnsResolver {
public:
    using RoutingTable = std::vector<sockaddr_in>;
    using SharedRoutingTable = std::shared_ptr<const RoutingTable>;

private:
    std::string hostname_;
    int port_;
    std::atomic<SharedRoutingTable> current_table_;
    std::thread background_thread_;
    std::atomic<bool> is_running_;

    void query_dns_once() noexcept {
        addrinfo hints{};
        hints.ai_family = AF_INET;       // Strictly IPv4 per specs
        hints.ai_socktype = SOCK_STREAM;
        
        addrinfo* result = nullptr;
        int status = ::getaddrinfo(hostname_.c_str(), nullptr, &hints, &result);
        
        if (status != 0) {
            // DNS resolution failed. Retain the old routing table automatically.
            return;
        }

        // 1. Data Immutability: Create an entirely new vector.
        auto new_table = std::make_shared<RoutingTable>();
        
        for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            if (ptr->ai_family == AF_INET) {
                // Copy the sockaddr_in before modifying—don't write to getaddrinfo-owned memory
                sockaddr_in addr = *reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
                addr.sin_port = htons(port_);
                new_table->push_back(addr);
            }
        }
        
        ::freeaddrinfo(result);

        // Standard: Implement a validation check ensuring new_table->size() > 0
        if (!new_table->empty()) {
            // 2. The Background Thread (Writer): Atomic Store.
            // Overwrite the global atomic pointer with the new std::shared_ptr.
            current_table_.store(new_table, std::memory_order_release);
        }
    }

    void run_loop() noexcept {
        while (is_running_.load(std::memory_order_acquire)) {
            query_dns_once();
            
            // Sleep interval of 5 seconds to prevent overwhelming the DNS service
            for (int i = 0; i < 50; ++i) { // 50 * 100ms = 5 seconds
                if (!is_running_.load(std::memory_order_acquire)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

public:
    // Create and start the dynamic headless DNS resolution engine
    DnsResolver(std::string hostname, int port)
        : hostname_(std::move(hostname)), port_(port), 
          current_table_(std::make_shared<RoutingTable>()), 
          is_running_(true) {
        
        // Synchronously execute the first query to ensure the routing table isn't 
        // empty the exact moment the balancer begins accepting connections.
        query_dns_once();

        // Spawn the dedicated background thread for RCU updates
        background_thread_ = std::thread([this]() { this->run_loop(); });
    }

    ~DnsResolver() {
        is_running_.store(false, std::memory_order_release);
        if (background_thread_.joinable()) {
            background_thread_.join();
        }
    }

    // Explicitly delete copy and move semantics.
    // This class owns a running std::thread capturing `this`—moving it would invalidate the pointer.
    DnsResolver(const DnsResolver&) = delete;
    DnsResolver& operator=(const DnsResolver&) = delete;
    DnsResolver(DnsResolver&&) = delete;
    DnsResolver& operator=(DnsResolver&&) = delete;
    
    // 3. The epoll Thread (Reader): Atomic Load
    // Lock-free method for the hot-path epoll loop to retrieve the active routes
    SharedRoutingTable get_routing_table() const noexcept {
        return current_table_.load(std::memory_order_acquire);
    }
};
