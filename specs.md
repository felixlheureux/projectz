# Architecture Specification: Polyglot Edge-to-Cloud Telemetry Pipeline (Projectz)

## 1. System Overview

A high-performance, distributed data ingestion system designed to process high-throughput telemetry from remote edge devices. This architecture employs a polyglot microservices model, leveraging the specific hardware and software sympathies of Rust, C++, and Go to eliminate I/O bottlenecks and maximize throughput.

**Performance Target:** Engineered to sustain a reliable ingestion and processing rate of 250,000 events/connections per second.

**Core Stack:**

- **Edge Agents (Rust):** Absolute memory safety and microscopic footprint for constrained embedded environments.
- **Layer 4 Load Balancer (C++):** Bare-metal, zero-garbage-collection network routing using Linux kernel primitives and strict modern C++ idioms.
- **Cloud Infrastructure (Go):** Massive network concurrency and scalable backend services.
- **Deployment:** Kubernetes (Docker/containerd), designed for an Ubuntu Linux host environment.
- **Architecture:** Monorepo.

---

## 2. Monorepo Structure

The repository is structured to manage three distinct build toolchains (Cargo, CMake, and Go Modules) while keeping protocol definitions synchronized.

```text
projectz/
├── agent/                 # Rust: Edge device data producers
│   ├── Cargo.toml
│   └── src/
├── core/                   # Shared Schemas: Protocol Buffers definitions
│   └── proto/              # .proto files used to generate Rust, C++, and Go bindings
├── services/
│   ├── loadbalancer/      # C++: Custom Layer 4 TCP/UDP proxy
│   │   ├── CMakeLists.txt
│   │   └── src/
│   ├── ingester/           # Go: API for receiving and validating edge payloads
│   ├── broker/             # Go: In-memory message queue with Dead Letter Queue (DLQ)
│   └── tsdb/               # Go: Custom time-series database storage engine
├── infrastructure/
│   ├── docker/             # Dockerfiles (multi-stage builds for Rust, C++, Go)
│   └── k8s/                # Kubernetes manifests (Deployments, Services, ConfigMaps)
└── scripts/                # Build automation (Cross-compiling Apple Silicon -> Linux)
```

---

## 3. Component Deep Dive (The Data Flow)

### Step 1: Data Producers (Edge Agents)

- **Implementation:** Rust.
- **Function:** Runs as a lightweight daemon alongside remote orchestration tools to collect hardware and system telemetry.
- **Behavior:** Batches data points and a cryptographic identity token into a compact binary format (Protocol Buffers). Manages a persistent, asynchronous TCP connection to the cloud balancer using `tokio`. Retains the payload buffer in local memory until an explicit application-layer acknowledgment (ACK) is received from the backend, utilizing exponential backoff for retries.

### Step 2: Layer 4 Load Balancer (The Front Door)

- **Implementation:** Modern C++ (C++20).
- **Kubernetes Role:** Exposed as a `LoadBalancer` or `NodePort` Service.
- **Function:** The high-speed traffic proxy. Accepts incoming edge connections, multiplexes bidirectional traffic, and routes transport-layer packets to backend pods.
- **Behavior & Systems Engineering Standards:** Operates strictly at the transport layer utilizing Linux's `epoll` in Edge-Triggered (`EPOLLET`) mode with non-blocking sockets.
  - **Bidirectional Multiplexing:** Monitors `EPOLLIN` for ingress telemetry and `EPOLLOUT` to route application-layer ACKs back to the specific Rust agent.
  - **RAII & Move Semantics:** Raw file descriptors are encapsulated in strictly managed object lifecycles. Socket ownership is transferred exclusively via move semantics (`&&`) to prevent FD leaks and duplication errors.
  - **Deterministic Memory Layout:** Utilizes stack-allocated contiguous memory (`std::array`) for the `epoll_event` buffer.
  - **Exception-Free Hot Path:** Error states are handled deterministically via `std::expected` or system error codes to avoid stack unwinding penalties.

### Step 3: Ingestion Pods (The Processors)

- **Implementation:** Go.
- **Kubernetes Role:** Deployed as a scalable `Deployment` (e.g., 3-5 replicas).
- **Function:** Terminates the connection, authenticates the payload, and decodes the Protobuf schema.
- **Behavior:** Exploits the Go scheduler by spawning a goroutine for every active connection. Performs in-memory validation of the embedded cryptographic token (avoiding Layer 7 API Gateway bottlenecks). Upon successful schema validation and queue insertion, generates the ACK response and transmits it back through the proxy.

### Step 4: Message Broker & Dead Letter Queue (The Buffer)

- **Implementation:** Go.
- **Kubernetes Role:** Deployed as a `StatefulSet`.
- **Function:** The buffer layer decoupling the fast network I/O of the Ingesters from the slower disk I/O of the Storage engine.
- **Behavior (Main Queue):** Implements a strictly lock-free ring buffer utilizing the `sync/atomic` package and Compare-And-Swap (CAS) operations to avoid `sync.Mutex` contention inherent to standard Go channels at 250k operations per second.
- **Behavior (Dead Letter Queue - DLQ):** Corrupted payloads, invalid signatures, or failed writes are routed here to prevent pipeline stalls.

### Step 5: Time-Series Storage (The Persistence Layer)

- **Implementation:** Go.
- **Kubernetes Role:** Deployed as a `StatefulSet` with a `PersistentVolumeClaim` (PVC) mapping to the Ubuntu host's block storage.
- **Function:** Long-term, highly optimized storage for sequential time-series data.
- **Behavior:** Background workers pull batches from the Message Broker. Data is buffered in memory (MemTable) and periodically flushed to disk in immutable, time-partitioned chunks (SSTables). This limits operations to sequential disk writes, allowing the storage engine to match the ingestion rate.

---

## 4. Development & Deployment Strategy

- **Protocol Buffers:** The `core/proto` directory serves as the source of truth. Protoc compilers generate the native Rust structs, C++ classes, and Go structs before any service compiles, guaranteeing contract alignment.
- **Cross-Compilation:** Docker `buildx` is utilized to cross-compile the target toolchains (`x86_64-unknown-linux-gnu` for Rust, and respective cross-compilers for C++ and Go) from the ARM64 development environment to generate native Linux binaries.
- **Kubernetes Networking:** The C++ load balancer monitors a headless Kubernetes `Service`. The C++ application implements a custom DNS resolution loop to periodically re-query CoreDNS, ensuring the active connection pool dynamically matches the actual IP addresses of the scaling Go Ingestion pods without relying on static caching.

---

## 5. Host Operating System Tuning (Ubuntu Linux)

To sustain 250,000 concurrent connections, the underlying host operating system requires explicit kernel parameter modifications to bypass default network and file descriptor limitations.

The following configurations must be applied via `/etc/sysctl.conf`:

- `fs.file-max = 1000000`: Increases the system-wide limit for open file descriptors.
- `net.core.somaxconn = 65535`: Increases the maximum number of queued connections allowed for a listening socket.
- `net.ipv4.tcp_max_syn_backlog = 65535`: Expands the queue size for incoming TCP SYN packets to prevent drops during traffic spikes.
- `net.ipv4.ip_local_port_range = 1024 65535`: Expands the available ephemeral port range for outbound connections.
