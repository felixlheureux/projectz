import ProxyServer;
#include <iostream>
#include <exception>

int main() {
    try {
        // Initialize the proxy server on port 8080.
        // The DNS resolver will target the local headless service.
        ProxyServer proxy(8080, "ingestion-headless.default.svc.cluster.local");
        
        // Enter the exception-free hot path
        proxy.run();

    } catch (const std::exception& e) {
        std::cerr << "[Loadbalancer] FATAL Initialization error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}