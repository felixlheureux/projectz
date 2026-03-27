import ProxyServer;
#include <iostream>
#include <exception>
#include <csignal>
#include <memory>

std::unique_ptr<ProxyServer> g_proxy;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[Loadbalancer] Received termination signal (" << signal << "). Gracefully shutting down..." << std::endl;
        if (g_proxy) {
            g_proxy->stop();
        }
    }
}

int main() {
    try {
        // Register POSIX signal handlers
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // Initialize the proxy server on port 8080.
        // The DNS resolver will target the local headless service.
        g_proxy = std::make_unique<ProxyServer>(8080, "ingestion-headless.default.svc.cluster.local");
        
        // Enter the exception-free hot path
        g_proxy->run();

        std::cout << "[Loadbalancer] Proxy shutdown complete." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[Loadbalancer] FATAL Initialization error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}