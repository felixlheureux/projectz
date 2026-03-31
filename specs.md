# Projectz: Polyglot Edge-to-Cloud Telemetry Pipeline

## Architecture Specification v0.5

---

## 1. What This System Does

Projectz is a high-throughput distributed telemetry pipeline. Remote edge devices (IoT sensors, embedded systems) continuously produce hardware and system telemetry. This system collects that data, validates its authenticity, buffers it against backpressure, and writes it to a purpose-built time-series database — all at a sustained rate of 250,000 events per second.

The architecture is polyglot by design. Each component is implemented in the language whose strengths match that component's constraints:

| Component                    | Language   | Why                                                                          |
| ---------------------------- | ---------- | ---------------------------------------------------------------------------- |
| Edge Agents                  | Rust       | Memory safety, zero runtime overhead, tiny binary for constrained devices    |
| Layer 4 Load Balancer        | C++        | Direct `io_uring` access, kernel-level network I/O, zero-copy routing        |
| Ingestion Pods               | Go         | Goroutine-per-connection concurrency, rapid prototyping, rich stdlib         |
| Message Broker (Queue + DLQ) | Rust       | Cache-line-aligned lock-free data structures, zero GC, deterministic latency |
| Time-Series Storage Engine   | Go         | High concurrency for read/write replicas, ergonomic disk I/O                 |
| Telemetry Dashboard          | Go + React | REST API over stored data, browser-based visualization for end users         |

The repository is a monorepo. The deployment target is Kubernetes on Ubuntu Linux.

---

## 2. Data Flow (End to End)

```
Edge Agent (Rust)
    │
    │  TCP + Protobuf envelope (binary, batched)
    ▼
L4 Load Balancer (C++)
    │
    │  Blind TCP proxy, fans out to pod IPs
    ▼
Ingestion Pods (Go, Deployment, 3-5 replicas)
    │
    │  gRPC stream (validated, decoded payloads)
    ▼
Message Broker (Rust, StatefulSet)
    │
    ├──→ Sharded SPSC Ring Buffers (main path)
    │       │
    │       │  gRPC pull (batched drain)
    │       ▼
    │     Storage Engine (Go, StatefulSet)
    │       │
    │       │  REST API (read replica)
    │       ▼
    │     Telemetry Dashboard (Go + React, Deployment)
    │       │
    │       ▼
    │     End User (browser)
    │
    ├──→ Retry Queue (transient write failures)
    │       │
    │       │  bounded retry with backoff
    │       ▼
    │     Storage Engine (re-attempt)
    │
    └──→ Poison Queue (unrecoverable garbage)
            │
            ▼
          Log, alert, discard
```

---

## 3. Component Specification

### 3.1 Edge Agents (Rust)

**Role:** Lightweight daemon running alongside device orchestration tools. Collects local hardware and system telemetry.

**Behavior:**

The agent batches telemetry data points into a Protobuf envelope. Each envelope includes a cryptographic identity token (Ed25519 signature over the payload) so the backend can verify both the sender's identity and data integrity without relying on transport-layer trust. Each envelope also carries a **priority field** — routine metrics (periodic temperature readings, heartbeats) are tagged `NORMAL`, while threshold breaches, error conditions, and anomaly detections are tagged `CRITICAL`. The priority assignment is configured per-metric on the agent and is part of the signed payload, so it cannot be tampered with in transit.

The agent maintains a persistent asynchronous TCP connection to the cloud load balancer via `tokio`. Payloads are retained in a local memory buffer until an explicit application-layer ACK is received from the ingestion pod. If no ACK arrives within a deadline, the agent retries with exponential backoff. This is the foundational guarantee against data loss — the edge device never discards data it hasn't been told was received.

**Key crate dependencies:** `tokio` (async runtime), `prost` (Protobuf codegen), `ed25519-dalek` (signing).

---

### 3.2 Layer 4 Load Balancer (C++)

**Role:** Bare-metal TCP proxy sitting at the cluster ingress. Routes raw byte streams from edge agents to ingestion pods. It has no knowledge of the payload content — it is a blind router.

**Why C++ and not a standard LB:** At 250,000 concurrent connections, conventional load balancers hit syscall overhead limits. This component uses Linux `io_uring` to eliminate context switches entirely on the data path:

- **Direct Descriptors:** Bypass the kernel's per-process file descriptor table. Socket handles are registered in a sparse, fixed-size descriptor table private to the `io_uring` instance, avoiding the `fget`/`fput` atomic reference counting overhead on every I/O operation.
- **Linked SQEs:** Receive and forward operations are submitted as linked pairs in the submission queue. The kernel executes them as a single atomic sequence — one syscall to drain the submission ring, zero intermediate wakeups.
- **Buffer Rings:** Pre-allocated receive buffers are registered in a shared ring that the kernel selects from directly during completion. Eliminates per-read buffer allocation and the associated `mmap`/`munmap` traffic.

**Backend Discovery:** The balancer monitors a headless Kubernetes `Service`. A dedicated thread runs a lock-free Read-Copy-Update (RCU) DNS resolution loop against CoreDNS, periodically refreshing the set of backend pod IPs. The RCU pattern ensures the `io_uring` submission thread never blocks on a DNS update — it always reads a consistent, immutable snapshot of the IP list, and stale snapshots are reclaimed once no reader holds a reference.

**TLS Strategy:** User-space TLS termination at 250k connections/second is a CPU bottleneck. TLS 1.3 is handled via one of two approaches:

1. **kTLS (Kernel TLS)** with AES-NI hardware offloading — the kernel handles encryption/decryption inline with socket I/O, keeping the hot path in kernel space.
2. **Hardware offload** — a dedicated TLS termination appliance or NIC sits in front of the proxy.

In either case, the C++ proxy itself never touches a TLS handshake.

---

### 3.3 Ingestion Pods (Go)

**Kubernetes resource:** `Deployment` (3-5 replicas, horizontally scalable).

**Role:** Terminate the TCP connection from the proxy, authenticate the payload, decode the Protobuf envelope, and forward validated data to the message broker.

**Inbound (from L4 proxy):** Raw TCP. The proxy forwards the byte stream untouched. Each pod accepts connections and spawns a goroutine per connection — this is where Go's runtime scheduler genuinely shines, managing hundreds of thousands of lightweight green threads without manual thread pool tuning.

**Outbound (to broker):** gRPC streaming. Each ingester maintains a persistent gRPC stream to the Rust broker. Validated payloads are forwarded over this stream, and the broker's response confirms queue insertion, which triggers the upstream ACK back to the edge agent.

**Zero-allocation decoding:** Incoming Protobuf payloads are decoded into pre-allocated structs recycled via `sync.Pool`. At 250k ops/second, heap allocation thrashing would produce GC latency spikes. The pool eliminates this — structs are borrowed, populated, forwarded, and returned. No new allocations on the hot path.

**Authentication:** Each Protobuf envelope embeds an Ed25519 token. The ingester performs in-memory signature verification. This is deliberately lightweight — no certificate chain traversal, no revocation checks, no external auth service calls. A single Ed25519 verify is ~70μs. Invalid signatures route the payload to the poison queue via the broker.

**Pre-aggregation:** Raw telemetry at full device reporting rate is too granular for the storage engine. If a device reports CPU temperature 100 times per second, the DB doesn't need 100 rows — it needs one row per interval containing min, max, mean, and count. The ingester aggregates validated payloads in-memory over a configurable time window (default: 1 second) per device per metric, then pushes a single aggregated payload to the broker. This reduces write volume by orders of magnitude without losing meaningful signal. `CRITICAL` priority payloads bypass aggregation entirely and are forwarded immediately — you never want to delay an alert.

**Adaptive sampling:** The broker periodically reports shard saturation (current depth / capacity) in its `InsertionAck` responses. When saturation exceeds a high watermark (e.g., 80%), the ingester enters degraded mode: the aggregation window widens (e.g., from 1s to 5s), and `NORMAL` payloads may be sampled (keep every Nth data point). When saturation drops below a low watermark (e.g., 50%), the ingester returns to full resolution. The edge agents are unaware of this — they keep sending at full rate, and still receive ACKs. The thinning happens entirely inside the ingester. This is the system's primary defense against sustained overproduction.

**ACK flow:**

1. Edge agent sends payload over TCP.
2. Ingester decodes and verifies.
3. Ingester pushes to broker via gRPC.
4. Broker confirms insertion.
5. Ingester sends ACK back over the TCP connection.
6. Edge agent clears the payload from its local buffer.

If any step fails, no ACK is sent. The edge agent retries. Data is never silently lost.

---

### 3.4 Message Broker (Rust)

**Kubernetes resource:** `StatefulSet`.

**Role:** The pressure valve between fast network I/O (ingesters) and slow disk I/O (storage engine). Absorbs bursts, drains at whatever rate the database can sustain.

**Why Rust (not Go):** This component's entire purpose is lock-free data structures with deterministic memory layout. The requirements are: cache-line-aligned atomic counters, guaranteed zero heap allocation on the hot path, and precise control over memory ordering semantics (Acquire/Release). Go can approximate all of these, but fights you on each one — `sync/atomic` provides weaker ordering guarantees, struct field alignment is not controllable to cache line granularity, and the GC can still trigger even with careful pooling. Rust gives you `#[repr(align(128))]`, `std::sync::atomic::Ordering`, and zero GC by construction.

#### 3.4.1 Topology: Sharded SPSC Ring Buffers

The broker does not use a single shared MPMC queue. Instead, it allocates one SPSC (Single-Producer, Single-Consumer) ring buffer per ingester connection.

**Why sharding over MPMC:**

In an MPMC design, every producer and consumer contends on shared `head` and `tail` atomic pointers. At 250k ops/second, this contention becomes the bottleneck — each CAS (Compare-And-Swap) retry loop burns CPU cycles and pollutes the cache.

Sharding eliminates this entirely. Each ring has exactly one writer (the goroutine handling one ingester's gRPC stream) and exactly one reader (a consumer worker). No contention, no CAS retry loops, no ABA risk.

- **SPSC** means Single-Producer, Single-Consumer — one writer, one reader, no conflicts.
- **MPMC** means Multi-Producer, Multi-Consumer — shared state, contention, complex correctness invariants.
- **CAS** is Compare-And-Swap: an atomic CPU instruction that updates a value only if it still holds an expected value. It's how lock-free structures avoid mutexes — but retries under contention waste cycles.
- **ABA problem:** A CAS bug where a value changes from A → B → A. The CAS sees the original A, assumes nothing changed, and succeeds — but the underlying state is different. SPSC avoids this because only one thread ever modifies each pointer.

**Ring buffer structure:**

```
┌───────────────────────────────────────────┐
│  Ring Buffer (fixed-size, pre-allocated)  │
│                                           │
│  [0] [1] [2] [3] [4] [5] [6] [7] ...     │
│        ▲                       ▲          │
│       tail                    head        │
│    (consumer)              (producer)     │
│                                           │
│  head advances on write (wraps around)    │
│  tail advances on read  (wraps around)    │
│  head == tail → empty                     │
│  head + 1 == tail → full                  │
└───────────────────────────────────────────┘
```

Each slot is a pre-allocated struct sized to hold one decoded telemetry payload. The buffer is allocated once at startup. Writes and reads are pointer advances — no allocation, no deallocation, no GC.

**Cache line isolation:** The `head` and `tail` counters must live on separate cache lines (128-byte alignment on modern x86, accounting for L1 line size + hardware prefetcher stride). Without this, writing `head` invalidates the cache line holding `tail`, forcing the consumer's CPU core to re-fetch it from shared cache — this is called false sharing and destroys throughput on tight loops.

```rust
#[repr(align(128))]
struct ProducerState {
    head: AtomicU64,
}

#[repr(align(128))]
struct ConsumerState {
    tail: AtomicU64,
}
```

**Memory ordering:** The producer writes data to the slot, then advances `head` with `Release` ordering. The consumer reads `head` with `Acquire` ordering before reading the slot. This guarantees the consumer always sees the fully written payload — never a half-written struct. No `SeqCst` (sequentially consistent) ordering is needed, which avoids a full memory fence on every operation.

#### 3.4.2 Flow Control Strategy (Three Layers)

A queue is a burst absorber, not a rate mismatch solution. If sustained production permanently exceeds the DB's write throughput, the ring buffers fill and the entire pipeline stalls. The system addresses this with three complementary mechanisms, listed from always-on to last-resort:

**Layer 1 — Pre-aggregation (always on, at the ingester):**

Described in Section 3.3. The ingester aggregates raw telemetry into per-second (or configurable interval) summaries before pushing to the broker. This is the single largest throughput reduction — 100 data points per device per second become 1 aggregated row. It runs unconditionally, regardless of system pressure.

**Layer 2 — Adaptive sampling (dynamic, triggered by shard saturation):**

The broker includes its current shard saturation percentage in every `InsertionAck` response. The ingester monitors this value:

- **Saturation > 80% (high watermark):** Ingester enters degraded mode. Aggregation window widens (1s → 5s → 15s). `NORMAL` payloads may be further downsampled (keep every Nth point). `CRITICAL` payloads are always forwarded immediately at full fidelity.
- **Saturation < 50% (low watermark):** Ingester returns to normal mode. Full resolution resumes.

The hysteresis gap between 80% and 50% prevents rapid oscillation between modes. Edge agents are completely unaware of adaptive sampling — they continue sending at full rate and receiving ACKs. The thinning is invisible to the producer.

**Layer 3 — Priority-aware shedding (last resort, at the broker):**

If adaptive sampling is insufficient and a shard reaches 100% capacity, the broker must decide what to do. Rather than blocking all producers equally (the original backpressure-only design), the broker uses the priority field embedded in each payload:

- **`CRITICAL` payloads:** Always accepted. If the ring is full, the broker evicts the oldest `NORMAL` payload to make room. Critical data — threshold breaches, error conditions, anomalies — is never shed.
- **`NORMAL` payloads:** Blocked (backpressure). The ingester's gRPC call stalls, no ACK is sent to the edge agent, and the agent retries with exponential backoff.

This means that under extreme sustained pressure, the system degrades gracefully: routine metrics slow down and may develop gaps, but critical alerts always flow through in real time. The edge agent's retry mechanism guarantees that once pressure subsides, `NORMAL` payloads eventually catch up — data is delayed, not lost.

**Why three layers instead of just backpressure:**

Backpressure alone (the original design) is correct but blunt. It treats all data equally and causes the entire pipeline to stall, including critical alerts, the moment any shard fills up. The three-layer approach means the system exhausts cheap options first (aggregation is free, sampling is nearly free) before resorting to blocking producers, and even when it blocks, it protects the data that matters most.

#### 3.4.3 gRPC Interface

**Inbound (from ingesters):** Bidirectional streaming. The ingester pushes validated payloads; the broker responds with insertion confirmations per-payload. The broker assigns each ingester stream to a dedicated SPSC shard.

**Outbound (to storage engine):** Server-side streaming. Storage workers open a pull stream and the broker drains batches from shards in round-robin order, distributing load evenly across available consumers.

**Proto contract (indicative):**

```protobuf
enum Priority {
  NORMAL = 0;
  CRITICAL = 1;
}

message TelemetryPayload {
  string agent_id = 1;
  int64 timestamp_ns = 2;
  Priority priority = 3;
  bytes ed25519_signature = 4;
  repeated MetricPoint metrics = 5;
}

message MetricPoint {
  string name = 1;
  double value = 2;
  // Present only in aggregated payloads (from ingester pre-aggregation)
  optional double min = 3;
  optional double max = 4;
  optional double mean = 5;
  optional uint32 count = 6;
}

message InsertionAck {
  uint64 sequence_id = 1;
  bool accepted = 2;
  // Broker reports shard pressure so ingesters can adapt
  float shard_saturation_pct = 3;
}

service Broker {
  // Ingesters push validated payloads, receive insertion confirmations
  // with shard saturation feedback for adaptive sampling
  rpc Ingest(stream TelemetryPayload) returns (stream InsertionAck);

  // Storage workers pull batches for writing
  rpc Drain(DrainRequest) returns (stream TelemetryBatch);

  // Operators inspect broker health
  rpc Status(Empty) returns (BrokerStatus);
}
```

**Crate dependencies:** `tonic` (gRPC), `prost` (Protobuf), `crossbeam` (lock-free primitives, optional — may hand-roll SPSC for portfolio demonstration).

---

### 3.5 Dead Letter Queue (Split Design)

The original spec lumped all failures into a single DLQ. This is a problem because it mixes fundamentally different failure modes: some are permanently broken, others are transient and retriable. The broker splits them.

#### 3.5.1 Poison Queue (Unrecoverable)

**Receives:** Corrupted payloads (malformed Protobuf), invalid Ed25519 signatures, schema version mismatches.

**Behavior:** Log the failure metadata (timestamp, source agent ID, failure reason, payload hash). Optionally persist a configurable window of raw payloads for forensic analysis. Emit a metric for alerting. No retry — these payloads are dead on arrival. No amount of retrying will fix a bad signature or garbled bytes.

**Implementation:** Append-only log file on a PVC, with structured JSON entries. A separate process or CronJob can rotate, compress, and archive these logs.

#### 3.5.2 Retry Queue (Transient Failures)

**Receives:** Payloads that were valid but failed to persist — storage engine timeout, write replica unavailable, disk pressure.

**Behavior:** Bounded retry with exponential backoff. Each payload gets a maximum retry count (e.g., 5 attempts). On each failure, the backoff interval doubles. If retries are exhausted, the payload is promoted to the poison queue with a failure reason of `max_retries_exceeded`, and an alert is emitted.

**Implementation:** A secondary ring buffer (or simple `VecDeque` — retry volume should be low relative to main throughput). A dedicated consumer goroutine drains it on a timer, re-submitting batches to the storage engine.

**Why this split matters:** Without it, a transient database hiccup floods the DLQ with retriable payloads, burying the genuinely broken ones. Operators can't distinguish "the DB was down for 30 seconds" from "an edge agent is sending garbage." The split makes both cases immediately legible.

---

### 3.6 Time-Series Storage Engine (Go)

**Kubernetes resource:** `StatefulSet` with `PersistentVolumeClaim` (PVC) mapped to host block storage. Read and write replicas.

**Role:** Long-term, sequential storage optimized for time-series access patterns (write-heavy, append-only, range-query reads).

**Write path:** Background workers open a gRPC pull stream to the broker, receiving batches of telemetry payloads. Incoming data is buffered in an in-memory table (MemTable). When the MemTable reaches a size threshold, it is flushed to disk as an immutable, time-partitioned chunk (SSTable). All disk operations are sequential writes — no random I/O, no seeks. This is what allows the storage engine to keep pace with the ingestion rate.

**Read path:** Range queries over time windows. A sparse in-memory index maps time ranges to SSTable files. Reads scan the relevant SSTables sequentially. Bloom filters on each SSTable can short-circuit lookups for missing keys.

**Replication:** Write replica receives all writes. Read replica(s) receive asynchronously replicated SSTables for serving queries without contending with the write path. Replication lag is acceptable for telemetry — queries on the read replica may be seconds behind the write frontier.

**Why Go (not Rust):** The storage engine's bottleneck is disk I/O, not CPU or memory layout. Go's goroutine model makes it straightforward to manage concurrent flush workers, compaction routines, replication streams, and query handlers. The GC overhead is negligible here because the hot data (MemTable) is a simple append-only structure with a predictable lifecycle — allocated, filled, flushed, discarded.

---

### 3.7 Telemetry Dashboard (Go API + React Frontend)

**Kubernetes resource:** `Deployment` (stateless, horizontally scalable).

**Role:** The user-facing surface of the entire system. Without this, data goes in and never comes out in a human-readable form. The dashboard answers the question every operator of an IoT fleet asks: "what are my devices doing right now, and what did they do over the last hour/day/week?"

This component has two parts: a Go REST API that queries the storage engine's read replica, and a React single-page application that renders the data in the browser.

#### 3.7.1 Query API (Go)

**Function:** A thin HTTP/JSON API that translates user-facing queries into range reads against the storage engine. It reads exclusively from the read replica — never the write replica — so query load cannot interfere with ingestion throughput.

**Endpoints (indicative):**

| Endpoint                         | Method | Description                                                                                      |
| -------------------------------- | ------ | ------------------------------------------------------------------------------------------------ |
| `/api/v1/devices`                | GET    | List all known edge agents (derived from agent IDs in stored payloads)                           |
| `/api/v1/devices/{id}/telemetry` | GET    | Time-range query for a specific device. Params: `start`, `end`, `interval` (downsampling bucket) |
| `/api/v1/devices/{id}/latest`    | GET    | Most recent telemetry payload for a device (live view)                                           |
| `/api/v1/fleet/summary`          | GET    | Aggregate fleet stats: active device count, total events ingested, error rates                   |
| `/api/v1/health`                 | GET    | API health check (storage connectivity, read replica lag)                                        |

**Downsampling:** Raw telemetry at 250k events/second is far too granular for a dashboard. The API supports an `interval` parameter (e.g., `1m`, `5m`, `1h`) that buckets data points and returns aggregates (min, max, mean, count) per interval. This is computed at query time by scanning the relevant SSTables and aggregating in memory. For frequently accessed time ranges, a caching layer (in-memory LRU or Redis) can reduce redundant scans.

**Authentication:** The API sits behind a Kubernetes `Ingress` or `Gateway`. Access control is handled at the ingress layer (API key, OAuth, or basic auth). The API itself is stateless — no sessions, no cookies. Every request carries its credentials.

**Implementation:** Go with `net/http` (standard library router — no framework needed for a handful of endpoints). JSON serialization via `encoding/json`. Queries to the storage engine over gRPC (reusing the existing `.proto` service definitions with a read-oriented RPC).

#### 3.7.2 Frontend (React)

**Function:** A single-page application served as a static bundle. The API serves it directly or it's deployed behind the same Ingress as the API. No server-side rendering — the browser does all the work.

**Core views:**

**Fleet Overview:** A table (or card grid) showing all registered edge devices with their last-seen timestamp, current status (reporting / stale / offline), and a sparkline of recent activity. This is the landing page — the operator sees fleet health at a glance.

**Device Detail:** Select a device and see its telemetry plotted over time. Time-series line charts for each metric the device reports (CPU temperature, memory usage, disk I/O, network throughput — whatever the Protobuf schema defines). Time range selector (last hour, last 24h, last 7d, custom). The charts pull from the downsampled API endpoint, so even week-long ranges render quickly.

**Live View:** A real-time tail of incoming telemetry for a selected device. The frontend polls the `/latest` endpoint on a short interval (1-2 seconds) or, if implemented, subscribes to a WebSocket/SSE stream. This is the "proof it works" view — you can watch data arrive from the edge agent in near-real-time.

**Charting library:** Recharts (lightweight, React-native, handles time-series well) or D3 for more control. The choice is cosmetic — either works.

**Why React (and not server-rendered Go templates):** The dashboard is interactive — time range selectors, device switching, live updates, chart zoom. A server-rendered approach would require full page reloads or awkward HTMX patterns for what is fundamentally a client-side interactive application. React is the standard tool for this. It also adds frontend skills to the portfolio signal, which complements the backend/infra depth.

#### 3.7.3 Separation from Grafana

The Grafana dashboards (Section 8) monitor the **pipeline infrastructure** — queue depth, ingestion rate, GC pressure, disk I/O. They answer "is the system healthy?"

The telemetry dashboard monitors the **data the pipeline carries** — device metrics, fleet status, historical trends. It answers "what are my devices reporting?"

These are different audiences (platform operator vs. IoT fleet operator), different data sources (Prometheus metrics vs. time-series storage), and different access patterns (debugging spikes vs. browsing historical telemetry). Keeping them separate is the correct design.

---

## 4. Intra-Cluster Communication

### 4.1 External Boundary (Edge → Cluster)

**Protocol:** Raw TCP carrying Protobuf-encoded binary envelopes.

**Why not gRPC here:** Edge devices are resource-constrained. gRPC requires HTTP/2 framing, header compression (HPACK), and a heavier client library. Raw TCP with a minimal Protobuf envelope keeps the agent binary small and the connection overhead negligible.

The L4 load balancer is a blind TCP proxy — it forwards bytes without inspection. gRPC would require the proxy to understand HTTP/2 frames for proper routing, which contradicts its role as a content-agnostic L4 forwarder.

### 4.2 Internal Boundary (Pod ↔ Pod)

**Protocol:** gRPC over HTTP/2.

**Why gRPC inside the cluster:**

- **Shared Protobuf contract.** The `.proto` files in `core/proto/` already define the data schema. gRPC service definitions live in the same files, making the API surface between components explicit, versioned, and compiler-checked across Rust (`tonic`/`prost`), Go (`google.golang.org/grpc`), and C++ (if needed).
- **Streaming.** The ingester → broker path is a persistent bidirectional stream (push payloads, receive insertion confirmations). The broker → storage path is a server-side stream (pull batches). gRPC streaming maps directly to both patterns.
- **Flow control.** HTTP/2 flow control provides backpressure at the transport layer, complementing the application-layer backpressure in the ring buffers.
- **Observability.** gRPC interceptors give you per-RPC latency, error rates, and tracing propagation with minimal code.

---

## 5. Security and Cryptography

Cryptographic workloads are decoupled from the data path to maintain throughput.

### 5.1 Transport Layer

TLS 1.3 is terminated before traffic reaches the C++ proxy, via kTLS with AES-NI hardware offloading or a dedicated TLS termination appliance. The proxy never performs a TLS handshake. Inside the cluster, pod-to-pod traffic uses mTLS managed by the service mesh or Kubernetes-native certificate rotation (e.g., cert-manager).

### 5.2 Application Layer

Zero-trust authentication is payload-level. Each Protobuf envelope carries an Ed25519 signature computed by the edge agent over the payload contents. The ingestion pods verify this signature in-memory — no external auth service, no certificate chain, no network hop. This is deliberately fast (~70μs per verify) and horizontally scalable (more ingester replicas = more verification throughput).

Invalid signatures are routed to the poison queue. The signature, agent ID, and timestamp are logged for forensic analysis.

---

## 6. Build and Deployment

### 6.1 Protobuf as Source of Truth

The `core/proto/` directory contains all `.proto` files defining both data schemas and gRPC service contracts. Before any service compiles, `protoc` generates:

- Rust structs and `tonic` service traits (via `prost-build` and `tonic-build`).
- Go structs and gRPC client/server stubs (via `protoc-gen-go` and `protoc-gen-go-grpc`).
- C++ classes (via standard `protoc` C++ codegen, if the proxy ever needs schema awareness).

This guarantees compile-time contract alignment across all three languages. A schema change that breaks any consumer fails the build, not production.

### 6.2 Cross-Compilation

Development is on ARM64 (Apple Silicon). Production targets are `x86_64-unknown-linux-gnu`. Docker `buildx` handles cross-compilation:

- **Rust:** `cross` or `cargo-zigbuild` targeting `x86_64-unknown-linux-gnu`.
- **Go:** `GOOS=linux GOARCH=amd64` — native cross-compilation.
- **C++:** Cross-toolchain in a multi-stage Dockerfile (`x86_64-linux-gnu-gcc`).

### 6.3 Kubernetes Resources

| Component              | Resource Type                 | Replicas   | Storage                                |
| ---------------------- | ----------------------------- | ---------- | -------------------------------------- |
| L4 Load Balancer       | `DaemonSet` or dedicated node | 1 (pinned) | None                                   |
| Ingestion Pods         | `Deployment`                  | 3-5 (HPA)  | None                                   |
| Message Broker         | `StatefulSet`                 | 1-3        | Ephemeral (ring buffers are in-memory) |
| Storage Engine (write) | `StatefulSet`                 | 1          | PVC → host block storage               |
| Storage Engine (read)  | `StatefulSet`                 | 1-2        | PVC → replicated SSTables              |
| Telemetry Dashboard    | `Deployment`                  | 1-2        | None (stateless)                       |
| Prometheus             | `StatefulSet`                 | 1          | PVC (15-day metric retention)          |
| Grafana                | `Deployment`                  | 1          | ConfigMap (provisioned dashboards)     |

---

## 7. Host OS Tuning (Ubuntu Linux)

At 250,000 concurrent connections, the default Linux kernel parameters are insufficient. The following must be applied via `/etc/sysctl.conf` and `limits.conf` on every node running the L4 proxy or ingestion pods.

### 7.1 File Descriptors and Sockets

```
fs.file-max = 1000000
```

System-wide ceiling on open file descriptors. The default (~65k) is exhausted almost immediately at target load.

```
net.core.somaxconn = 65535
```

Maximum backlog for listening sockets. When a SYN arrives faster than `accept()` can drain the queue, excess connections are dropped. This raises the ceiling.

```
net.ipv4.tcp_max_syn_backlog = 65535
```

Kernel queue depth for half-open TCP connections (SYN received, SYN-ACK sent, waiting for final ACK). Under SYN flood or burst traffic, this prevents silent drops.

```
net.ipv4.ip_local_port_range = 1024 65535
```

Ephemeral port range for outbound connections. The proxy opens connections to backend pods — each one consumes a port. The default range (~28k ports) limits outbound concurrency.

### 7.2 Memory and Hugepages

```
vm.nr_hugepages = 1024
```

Pre-allocates 1024 × 2MB hugepages. The `io_uring` buffer rings (`io_uring_setup_buf_ring`) map these pages for packet reception buffers. Hugepages reduce TLB (Translation Lookaside Buffer) cache misses — each TLB entry covers 2MB instead of 4KB, meaning fewer page table walks during high-throughput I/O.

**RLIMIT_MEMLOCK:** The proxy container must run with `memlock` privileges (`ulimit -l unlimited`) so `io_uring` can pin buffer memory in kernel space without hitting the default per-process lock limit.

---

## 8. Observability (Prometheus + Grafana)

Observability is not an afterthought — it's the proof that the system works. A Grafana dashboard showing live throughput under load is the single most compelling artifact in a portfolio. Every component exposes metrics via Prometheus, and Grafana renders them into dashboards that make the system's behavior legible at a glance.

### 8.1 Metrics Architecture

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  Edge Agent   │     │   Ingester   │     │    Broker    │
│  (Rust)       │     │   (Go)       │     │   (Rust)     │
│  :9100/metrics│     │  :9200/metrics│    │  :9300/metrics│
└──────┬───────┘     └──────┬───────┘     └──────┬───────┘
       │                    │                    │
       │         ┌──────────┴────────┐           │
       │         │                   │           │
       ▼         ▼                   ▼           ▼
     ┌─────────────────────────────────────────────┐
     │           Prometheus (scrape loop)           │
     │           StatefulSet + PVC                  │
     │           scrape_interval: 5s                │
     └──────────────────┬──────────────────────────┘
                        │
                        │  PromQL queries
                        ▼
     ┌─────────────────────────────────────────────┐
     │           Grafana (dashboards)               │
     │           Deployment                         │
     │           :3000                              │
     └─────────────────────────────────────────────┘
```

Each component exposes a `/metrics` HTTP endpoint in Prometheus exposition format. Prometheus scrapes all endpoints on a 5-second interval via Kubernetes service discovery (`kubernetes_sd_configs` with the `pod` role). Grafana queries Prometheus via PromQL and renders dashboards.

**Kubernetes resources:**

| Component  | Resource Type | Storage                                  |
| ---------- | ------------- | ---------------------------------------- |
| Prometheus | `StatefulSet` | PVC (metrics retention, default 15 days) |
| Grafana    | `Deployment`  | ConfigMap (dashboard JSON provisioning)  |

### 8.2 Metrics Per Component

#### Edge Agents (Rust)

Exposed via the `prometheus` crate with a lightweight Hyper HTTP server on a dedicated port.

| Metric                      | Type      | What It Tells You                                         |
| --------------------------- | --------- | --------------------------------------------------------- |
| `edge_payloads_sent_total`  | Counter   | Total payloads transmitted to the cloud                   |
| `edge_acks_received_total`  | Counter   | Total ACKs received (delta with sent = in-flight)         |
| `edge_retries_total`        | Counter   | Retry count (sustained increase = downstream problem)     |
| `edge_payload_batch_size`   | Histogram | Distribution of batch sizes (tuning signal)               |
| `edge_send_latency_seconds` | Histogram | Round-trip time from send to ACK                          |
| `edge_buffer_depth`         | Gauge     | Payloads waiting in local buffer (backpressure indicator) |

#### L4 Load Balancer (C++)

Exposed via a minimal embedded HTTP handler (or a sidecar exporter if embedding HTTP in the io_uring event loop is undesirable).

| Metric                          | Type    | What It Tells You                                     |
| ------------------------------- | ------- | ----------------------------------------------------- |
| `lb_active_connections`         | Gauge   | Current open TCP connections                          |
| `lb_connections_accepted_total` | Counter | Total connections accepted                            |
| `lb_connections_failed_total`   | Counter | Connections dropped (SYN backlog full, fd exhaustion) |
| `lb_bytes_forwarded_total`      | Counter | Total bytes proxied (throughput proof)                |
| `lb_backend_pool_size`          | Gauge   | Number of healthy backend pod IPs from DNS            |
| `lb_sqe_submissions_total`      | Counter | io_uring submission queue entries submitted           |
| `lb_cqe_completions_total`      | Counter | io_uring completion queue entries reaped              |

#### Ingestion Pods (Go)

Exposed via the standard `prometheus/client_golang` library. gRPC interceptors automatically capture per-RPC metrics.

| Metric                                       | Type      | What It Tells You                                              |
| -------------------------------------------- | --------- | -------------------------------------------------------------- |
| `ingester_payloads_received_total`           | Counter   | Total payloads received from proxy                             |
| `ingester_payloads_validated_total`          | Counter   | Payloads that passed Ed25519 verification                      |
| `ingester_payloads_rejected_total`           | Counter   | Failed signature verification (→ poison queue)                 |
| `ingester_auth_latency_seconds`              | Histogram | Ed25519 verification time distribution                         |
| `ingester_broker_push_latency_seconds`       | Histogram | Time to push to broker and receive insertion ACK               |
| `ingester_pool_recycled_total`               | Counter   | sync.Pool struct reuse count (GC avoidance proof)              |
| `ingester_pool_allocated_total`              | Counter   | New allocations (should flatline after warmup)                 |
| `ingester_aggregation_window_seconds`        | Gauge     | Current aggregation window (widens under pressure)             |
| `ingester_aggregation_ratio`                 | Gauge     | Raw payloads in / aggregated payloads out (compression factor) |
| `ingester_sampling_mode`                     | Gauge     | 0 = normal, 1 = degraded (adaptive sampling active)            |
| `ingester_payloads_sampled_out_total`        | Counter   | Payloads dropped by adaptive sampling (data resolution lost)   |
| `ingester_critical_payloads_forwarded_total` | Counter   | CRITICAL payloads forwarded (should never be sampled)          |

#### Message Broker (Rust)

Exposed via the `prometheus` crate. This is the most critical metrics surface — it's where you prove the lock-free design works.

| Metric                             | Type              | What It Tells You                                     |
| ---------------------------------- | ----------------- | ----------------------------------------------------- |
| `broker_shard_count`               | Gauge             | Active SPSC ring buffer shards                        |
| `broker_shard_depth`               | Gauge (per shard) | Slots occupied per shard (backpressure signal)        |
| `broker_shard_capacity`            | Gauge (per shard) | Total slots per shard (depth/capacity = saturation %) |
| `broker_enqueue_total`             | Counter           | Total successful enqueues                             |
| `broker_dequeue_total`             | Counter           | Total successful dequeues                             |
| `broker_backpressure_events_total` | Counter           | Times a NORMAL producer blocked on a full ring        |
| `broker_critical_evictions_total`  | Counter           | NORMAL payloads evicted to make room for CRITICAL     |
| `broker_critical_enqueue_total`    | Counter           | CRITICAL payloads accepted (should never be rejected) |
| `broker_poison_queue_depth`        | Gauge             | Unrecoverable failures awaiting forensic review       |
| `broker_retry_queue_depth`         | Gauge             | Transient failures awaiting retry                     |
| `broker_retry_attempts_total`      | Counter           | Total retry attempts                                  |
| `broker_retry_exhausted_total`     | Counter           | Payloads promoted from retry → poison                 |

#### Storage Engine (Go)

| Metric                          | Type      | What It Tells You                    |
| ------------------------------- | --------- | ------------------------------------ |
| `storage_writes_total`          | Counter   | Total successful SSTable writes      |
| `storage_write_failures_total`  | Counter   | Failed writes (→ broker retry queue) |
| `storage_write_latency_seconds` | Histogram | Flush-to-disk latency                |
| `storage_memtable_size_bytes`   | Gauge     | Current MemTable fill level          |
| `storage_sstable_count`         | Gauge     | Total SSTables on disk               |
| `storage_compaction_runs_total` | Counter   | Compaction cycles completed          |
| `storage_disk_usage_bytes`      | Gauge     | Total PVC consumption                |

### 8.3 Grafana Dashboards

Dashboards are provisioned as JSON files via ConfigMap, loaded automatically on Grafana startup. Three dashboards cover the full system:

**Dashboard 1: Pipeline Overview (the hero dashboard)**

The one-screen summary for a portfolio demo or README screenshot. Top row: four big-number panels showing events/sec ingested, end-to-end p99 latency, active connections, and error rate. Second row: a throughput time-series graph (stacked area: ingested → queued → written) proving data flows end-to-end. Third row: queue saturation gauge (ring buffer depth / capacity), DLQ counters, and the aggregation compression ratio. Bottom row: flow control status — a traffic-light indicator showing current sampling mode (normal / degraded), the current aggregation window, and the critical eviction rate.

**Dashboard 2: Component Deep Dive**

One row per component. Each row shows that component's key metrics: the LB's connection count and byte throughput, the ingesters' validation rate, pool efficiency, and aggregation ratio, the broker's per-shard depth heatmap with priority breakdown, and the storage engine's write latency and disk usage. This is where you demonstrate you understand what to measure and why.

**Dashboard 3: Alerts and Anomalies**

Panels specifically for failure modes: sustained backpressure events, poison queue growth, retry exhaustion rate, ingester pool allocation spikes (GC pressure returning), storage write failure bursts, and adaptive sampling activation history. Each panel has a threshold annotation line showing "healthy" vs. "investigate."

### 8.4 Alerting Rules (Prometheus Alertmanager)

Defined in a `PrometheusRule` CRD or a rules file. These are examples — thresholds should be tuned under load testing:

```yaml
groups:
  - name: projectz-alerts
    rules:
      - alert: BrokerBackpressureSustained
        expr: rate(broker_backpressure_events_total[5m]) > 0
        for: 2m
        labels:
          severity: warning
        annotations:
          summary: 'Broker ring buffer full — ingesters are blocking'

      - alert: PoisonQueueGrowing
        expr: broker_poison_queue_depth > 100
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: 'Poison queue accumulating — possible bad actor or schema mismatch'

      - alert: RetryExhaustionSpike
        expr: rate(broker_retry_exhausted_total[5m]) > 1
        for: 1m
        labels:
          severity: critical
        annotations:
          summary: 'Payloads failing all retry attempts — storage engine may be down'

      - alert: StorageWriteLatencyHigh
        expr: histogram_quantile(0.99, rate(storage_write_latency_seconds_bucket[5m])) > 0.1
        for: 3m
        labels:
          severity: warning
        annotations:
          summary: 'Storage p99 write latency above 100ms — disk pressure or compaction lag'

      - alert: AdaptiveSamplingActive
        expr: ingester_sampling_mode == 1
        for: 1m
        labels:
          severity: warning
        annotations:
          summary: 'Ingester in degraded mode — data resolution reduced due to broker pressure'

      - alert: CriticalEvictionSpike
        expr: rate(broker_critical_evictions_total[5m]) > 10
        for: 1m
        labels:
          severity: critical
        annotations:
          summary: 'Broker evicting NORMAL payloads for CRITICAL — sustained overload'
```

### 8.5 Implementation Notes

**Rust components** (edge agent, broker): Use the `prometheus` crate. Metrics are registered in a global `Registry` and served over a dedicated HTTP port via a lightweight `hyper` server running on a separate tokio task. This keeps the metrics endpoint off the hot data path.

**Go components** (ingesters, storage): Use `prometheus/client_golang`. gRPC server and client interceptors (`grpc-ecosystem/go-grpc-prometheus`) automatically emit per-RPC latency histograms and error counters with zero manual instrumentation.

**C++ load balancer:** Either embed a minimal HTTP handler on a separate thread (not on the io_uring event loop — that would introduce syscall overhead on the fast path), or run a Prometheus exporter as a sidecar container that reads shared-memory metrics written by the proxy. The sidecar approach is cleaner but adds deployment complexity.

**Grafana provisioning:** Dashboard JSON files and Prometheus datasource configuration are stored in the monorepo under `deploy/grafana/` and mounted as ConfigMaps. On pod startup, Grafana auto-imports them — no manual dashboard creation.

---

## 9. Open Design Questions

The following are not yet resolved and should be addressed before implementation begins:

1. **Broker shard rebalancing.** When an ingester pod scales up or down, how are SPSC shards assigned and drained? Does the broker pre-allocate a fixed shard count, or dynamically create/destroy them? What happens to in-flight data in a shard when its ingester disconnects?

2. **Broker persistence.** The ring buffers are in-memory. If the broker pod restarts, all buffered data is lost. The edge agents will retry (they haven't received ACKs), but this creates a re-ingestion storm. Should the broker optionally WAL (write-ahead log) to disk for crash recovery?

3. **Storage engine compaction.** SSTables accumulate over time. A background compaction process must merge and tombstone old data. The compaction strategy (size-tiered vs. leveled) affects both read amplification and write amplification.

4. **Capacity planning.** At 250k events/second with an average payload size of N bytes, what is the sustained write bandwidth to disk? What is the ring buffer memory footprint per shard? These numbers determine PVC sizing and node memory requirements.
