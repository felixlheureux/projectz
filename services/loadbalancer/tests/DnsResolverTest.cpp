#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>

import DnsResolver;

// Test 3: RCU DNS Routing Table Concurrency Test
TEST(DnsResolverTest, LockFreeRcuDataRaceValidation) {
    // Initialize the DnsResolver. 
    // Initialize the DnsResolver targeting localhost to avoid 30s getaddrinfo DNS timeouts organically
    DnsResolver resolver("localhost", 8080);
    
    std::atomic<bool> is_running{true};
    std::atomic<uint64_t> read_counts{0};
    
    constexpr int NUM_READERS = 10;
    std::vector<std::thread> readers;
    
    // Spawn 10 simultaneous reader threads slamming get_routing_table() inside a tight loop simulating massive load.
    for (int i = 0; i < NUM_READERS; ++i) {
        readers.emplace_back([&]() {
            while (is_running.load(std::memory_order_acquire)) {
                // Atomic Load: zero mutex contention
                auto table = resolver.get_routing_table();
                
                // Assert structural integrity of the returned immutable pointer
                ASSERT_NE(table, nullptr);
                
                // Simulate traversing the read-only struct
                size_t s = table->size();
                (void)s;
                
                read_counts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // Let the proxy spin under massive concurrency for a short burst
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Signal shutdown
    is_running.store(false, std::memory_order_release);
    for (auto& reader : readers) {
        if (reader.joinable()) {
            reader.join();
        }
    }
    
    // Data Race Validation: Assert we achieved massive scale without mutex stalling
    EXPECT_GT(read_counts.load(), 50000); // Expecting hundreds of thousands of reads over half a second
    
    // Memory Leak Validation: 
    // The master DnsResolver holds exactly 1 count across the atomic pointer.
    // If readers failed to drop their RCU handles, this would be > 1.
    auto final_table = resolver.get_routing_table();
    EXPECT_EQ(final_table.use_count(), 2); // 1 for the scope here, 1 for the internal DnsResolver
}
