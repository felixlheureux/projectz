# Architecture Specification: Polyglot Edge-to-Cloud Telemetry Pipeline (Projectz)

## 1. System Overview

A high-performance, distributed data ingestion system designed to process high-throughput telemetry from remote edge devices. This architecture employs a polyglot microservices model, leveraging the specific hardware and software sympathies of Rust, C++, and Go to eliminate I/O bottlenecks and maximize throughput.

**Performance Target:** Engineered to sustain a reliable ingestion and processing rate of 250,000 events/connections per second over a zero-context-switch fast-path.

**Core Stack:**

- **Edge Agents (Rust):** Absolute memory safety and microscopic footprint for constrained embedded environments.
- **Layer 4 Load Balancer (C++):** Bare-metal network routing utilizing advanced `io_uring` topologies (Direct Descriptors, Linked SQEs, Buffer Rings) and strict modern C++ idioms.
- **Cloud Infrastructure (Go):** Massive network concurrency and scalable backend services with zero-allocation memory pools.
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
│   ├── loadbalancer/      # C++: Custom Layer 4 TCP/UDP proxy (io_uring)
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

- **Implementation:** Modern C++ (C++23).
- **Kubernetes Role:** Exposed as a `LoadBalancer` or `NodePort` Service.
- **Function:** The high-speed traffic proxy. Accepts incoming edge connections, multiplexes bidirectional traffic, and routes transport-layer packets to backend pods entirely within the kernel context.
- **Behavior & Systems Engineering Standards:** Operates strictly at the transport layer, utilizing advanced `io_uring` orchestration to achieve a zero-context-switch architecture, expressly disabling `SQPOLL` to prevent interrupt cache-thrashing.
  - **Modern Ring Topology:** Initializes the ring with `IORING_SETUP_SINGLE_ISSUER`, `IORING_SETUP_DEFER_TASKRUN`, and `IORING_SETUP_COOP_TASKRUN`. This forces the kernel to bypass internal mutexes and defers all `task_work` processing to the synchronous `io_uring_submit_and_wait` boundary, eliminating destructive Inter-Processor Interrupts (IPIs).
  - **VFS Lock Bypass via Direct Descriptors:** Abandons the global file descriptor table (which suffers severe spinlock contention). Registers a sparse file table (`io_uring_register_files_sparse`) and utilizes `IORING_FILE_INDEX_ALLOC` alongside `IORING_OP_SOCKET` to allocate sockets completely asynchronously.
  - **Atomic Backend Routing (Linked SQEs):** Submits chained operations via `IOSQE_IO_LINK`. A single batch submission natively creates the direct socket (`IORING_OP_SOCKET`), configures it (`IORING_OP_URING_CMD` for `SOCKET_URING_OP_SETSOCKOPT`), and establishes the connection (`IORING_OP_CONNECT`). If one fails, the chain aborts securely without returning to userspace.
  - **Ring-Mapped Provided Buffers:** Deprecates static memory slabs in favor of `io_uring_setup_buf_ring`. Memory is leased dynamically to sockets exclusively upon physical packet arrival. The buffer ring is backed by `mmap` with `MAP_HUGETLB` (hugepages) to eliminate Translation Lookaside Buffer (TLB) misses during DMA transfers.
  - **Asymmetric Overflow Mitigation:** Sizing the Completion Queue (e.g., 32,768) asymmetrically larger than the Submission Queue (e.g., 8,192). The event loop explicitly monitors for `IORING_SQ_CQ_OVERFLOW` to drain the `cq_overflow_list` dynamically under heavy multishot accept bursts.
  - **Ephemeral Port Exhaustion Defenses:** Overcomes the mathematical limits of the 64,500 ephemeral port limit via two mechanisms:
    1.  **Loopback IP Multiplexing:** Outbound proxy connections round-robin their bind addresses across the `127.0.0.0/8` subnet (e.g., `127.0.0.2` to `127.0.0.20`), scaling the 4-tuple space to millions of available combinations.
    2.  **`SO_LINGER` Active Resets:** Abandons graceful `FIN` states. Embeds `SO_LINGER` (linger=0) via an asynchronous `URING_CMD` prior to closure, forcing the TCP stack to transmit an `RST` packet. This instantly evicts the connection from `conntrack`, entirely bypassing the 60-second `TIME_WAIT` penalty.

### Step 3: Ingestion Pods (The Processors)

- **Implementation:** Go.
- **Kubernetes Role:** Deployed as a scalable `Deployment` (e.g., 3-5 replicas).
- **Function:** Terminates the connection, authenticates the payload, and decodes the Protobuf schema.
- **Behavior:** Exploits the Go scheduler by spawning a goroutine for every active connection.
  - **Zero-Allocation Decoding:** Utilizes `sync.Pool` to recycle pre-allocated memory structs for incoming Protobuf payloads. This prevents heap allocation thrashing and completely eliminates Garbage Collection (GC) latency spikes at 250k operations/second.
  - **Inline Authentication:** Performs in-memory validation of the embedded Ed25519 cryptographic token. Upon successful validation and queue insertion, generates the ACK response and transmits it back through the proxy.

### Step 4: Message Broker & Dead Letter Queue (The Buffer)

- **Implementation:** Go.
- **Kubernetes Role:** Deployed as a `StatefulSet`.
- **Function:** The buffer layer decoupling the fast network I/O of the Ingesters from the slower disk I/O of the Storage engine.
- **Behavior (Main Queue):** Implements a strictly lock-free ring buffer utilizing the `sync/atomic` package and Compare-And-Swap (CAS) operations to avoid `sync.Mutex` contention inherent to standard Go channels at high throughput.
- **Behavior (Dead Letter Queue - DLQ):** Corrupted payloads, invalid signatures, or failed writes are routed here to prevent pipeline stalls.

### Step 5: Time-Series Storage (The Persistence Layer)

- **Implementation:** Go.
- **Kubernetes Role:** Deployed as a `StatefulSet` with a `PersistentVolumeClaim` (PVC) mapping to the Ubuntu host's block storage.
- **Function:** Long-term, highly optimized storage for sequential time-series data.
- **Behavior:** Background workers pull batches from the Message Broker. Data is buffered in memory (MemTable) and periodically flushed to disk in immutable, time-partitioned chunks (SSTables). This limits operations to sequential disk writes, allowing the storage engine to match the ingestion rate.

---

## 4. Security & Cryptography Strategy

To maintain high throughput without compromising data integrity or security, cryptographic workloads are strictly decoupled and optimized.

- **Transport Layer Security (TLS):** Standard user-space TLS termination (e.g., OpenSSL) is a severe CPU bottleneck at 250,000 connections/second. TLS 1.3 is managed via **kTLS (Kernel TLS)** with AES-NI hardware cryptographic offloading, or entirely delegated to a dedicated hardware load balancer preceding the C++ proxy.
- **Application Layer Authentication:** The proxy acts as a blind TCP router. Zero-trust authentication is achieved via lightweight Ed25519 tokens embedded directly within the Protobuf envelopes, validated asynchronously by the horizontally scaling Go pods.

---

## 5. Development & Deployment Strategy

- **Protocol Buffers:** The `core/proto` directory serves as the source of truth. Protoc compilers generate the native Rust structs, C++ classes, and Go structs before any service compiles, guaranteeing contract alignment.
- **Cross-Compilation:** Docker `buildx` is utilized to cross-compile the target toolchains (`x86_64-unknown-linux-gnu` for Rust, and respective cross-compilers for C++ and Go) from the ARM64 development environment to generate native Linux binaries.
- **Kubernetes Networking:** The C++ load balancer monitors a headless Kubernetes `Service`. The application implements a custom, lock-free Read-Copy-Update (RCU) DNS resolution loop to periodically re-query CoreDNS, ensuring the connection pool dynamically matches backend IPs without stalling the `io_uring` thread.

---

## 6. Host Operating System Tuning (Ubuntu Linux)

To sustain 250,000 concurrent connections via `io_uring` without resource exhaustion or page faulting, the underlying host OS requires strict kernel parameter tuning.

The following configurations must be applied via `/etc/sysctl.conf` and `limits.conf`:

- `fs.file-max = 1000000`: Increases the system-wide limit for open file descriptors (applies to standard FDs outside the proxy's sparse direct descriptor table).
- `net.core.somaxconn = 65535`: Increases the maximum number of queued connections allowed for a listening socket.
- `net.ipv4.tcp_max_syn_backlog = 65535`: Expands the queue size for incoming TCP SYN packets to prevent drops during traffic spikes.
- `net.ipv4.ip_local_port_range = 1024 65535`: Expands the available ephemeral port range for outbound connections.
- `vm.nr_hugepages = 1024`: Allocates 2MB hugepages to be mapped by `io_uring_setup_buf_ring`, drastically reducing TLB cache misses during packet reception.
- **RLIMIT_MEMLOCK:** The proxy container must be granted `memlock` privileges (`ulimit -l unlimited`) to allow `io_uring` to pin memory pages in kernel space safely.
