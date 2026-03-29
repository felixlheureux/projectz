# Architecture Specification: Polyglot Edge-to-Cloud Telemetry Pipeline (Projectz)

## 1. System Overview

A high-performance, distributed data ingestion system designed to process high-throughput telemetry from remote edge devices. This architecture employs a polyglot microservices model, leveraging the specific hardware and software sympathies of Rust, C++, and Go to eliminate I/O bottlenecks and maximize throughput.

**Performance Target:** Engineered to sustain a reliable ingestion and processing rate of 250,000 events/connections per second over a zero-context-switch fast-path.

**Core Stack:**

- **Edge Agents (Rust):** Absolute memory safety and microscopic footprint for constrained embedded environments.
- **Layer 4 Load Balancer (C++):** Bare-metal network routing utilizing advanced `io_uring` topologies (Direct Descriptors, Linked SQEs, Buffer Rings).
- **Cloud Infrastructure (Go):** Massive network concurrency and scalable backend services with zero-allocation memory pools.
- **Deployment:** Kubernetes (Docker/containerd), designed for an Ubuntu Linux host environment.
- **Architecture:** Monorepo.

---

## 3. Component Deep Dive (The Data Flow)

### Step 1: Data Producers (Edge Agents)

- **Implementation:** Rust.
- **Function:** Runs as a lightweight daemon alongside remote orchestration tools to collect hardware and system telemetry.
- **Behavior:** Batches data points and a cryptographic identity token into a compact binary format (Protocol Buffers). Manages a persistent, asynchronous TCP connection to the cloud balancer using `tokio`. Retains the payload buffer in local memory until an explicit application-layer acknowledgment (ACK) is received from the backend, utilizing exponential backoff for retries.

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
