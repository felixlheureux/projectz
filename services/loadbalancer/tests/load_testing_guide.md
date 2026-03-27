# Layer 4 Load Balancer: Load Testing & Saturation Guide

Unit testing and sanitizers can validate logical correctness, but a Layer 4 proxy aiming for 250,000 active concurrent connections/second *must* be validated directly against the Linux Kernel.

This guide provides exactly how to replicate standard high-concurrency exhaustion tests securely within your target Ubuntu deployment ring.

## Prerequisites: Kernel Tuning
Before triggering tests, ensure your host has fundamentally lifted the file descriptor restrictions (detailed inside `specs.md`):
```bash
sudo sysctl -w fs.file-max=2000000
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65000"
ulimit -n 1000000
```

---

## 1. Saturation Testing (Native TCP Spammer)
To validate the EPOLLET looping and strict core routing throughput, you must exhaust the TCP stack. We use the custom built `scripts/tcp_spammer.cpp` orchestrator via `load_test.sh` because this is a Layer 4 proxy with no HTTP routing logic to answer external tools like `wrk`.

**Validate High Connections Matrix:**
*Start the proxy in the background:*
```bash
make run-release &
```
*Run the native load testing suite:*
```bash
make load-test
```
*Metrics Check:* Our custom script will compile and saturate the active worker loops, measuring pure generic TCP connections per second (CPS) established.

---

## 2. Heap Tracking Profiling (`valgrind / massif`)
The `ProxyServer.cppm` architecture completely avoids `new` or `malloc` during the hot path. To mathematically prove zero steady-state memory fragmentation over an hour:

*Execute proxy natively under Massif:*
```bash
valgrind --tool=massif --pages-as-heap=yes ./loadbalancer
```
*Send saturation traffic:* Run the `wrk` load script above.
*Terminate and Analyze:*
```bash
ms_print massif.out.<pid>
```
*Metrics Check:* Mathematical validation is achieved if the printed heap graph plateaus entirely flat during load, proving all processing executes internally against stack-allocated `std::array<epoll_event>`.

---

## 3. TCP State Exhaustion (`hping3` SYN Flodding)
Validating that the proxy defers correctly to `tcp_syncookies` during half-open TCP storms. Without exceptions intercepting bad packets, our module prevents exhausting our 1M `MAX_FD` array.

*Execute simulated SYN Flood targeted at our ingress port:*
```bash
sudo hping3 -S --flood -V -p 8080 127.0.0.1
```
*Metrics Check:* Keep `top -p $(pidof loadbalancer)` active. If the kernel drops the SYNs instead of instantiating active `Socket` bindings, process RAM remains locked natively under 50MB, completely surviving the attack vector.
