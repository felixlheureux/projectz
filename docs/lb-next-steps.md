# Load Balancer — Production Gaps & Next Steps

Current state: the LB is functionally correct (io_uring splice forwarding, health checks,
self-registration, KEDA autoscaling, Prometheus metrics). The gaps below must be addressed
before this can be considered production-grade.

---

## 1. Logging

**Problem:** Every accepted connection emits a log line:

```text
Proxying: client_fd=2503 -> 10.244.0.15:6000 active=414 queued=0
```

At 250,000 connections/second this is 250k log lines/second. `std::cout` under thread
contention becomes a bottleneck, and any log shipper (Fluentbit, Vector) will be overwhelmed.

**Standard:** Structured JSON to stdout, errors only at runtime, sampled access logs.
Stdout JSON is the correct pattern in k8s — the container runtime captures it and the
log shipper forwards it to the backend (Loki, Elasticsearch, Datadog).

**Implementation:**

- Add [`spdlog`](https://github.com/gabime/spdlog) — async, structured JSON, near-zero
  overhead when a log level is disabled. Add to `CMakeLists.txt` via `FetchContent`.
- Replace all `std::cout` with `spdlog::info` / `spdlog::error`.
- Move the per-connection `Proxying:` line to `spdlog::debug` — compiled out in release builds.
- Log only: backend registration/deregistration, health state transitions, ring exhaustion,
  graceful shutdown events.

**Example output (JSON):**

```json
{"ts":"2026-03-29T19:41:25Z","level":"info","msg":"backend registered","ip":"10.244.0.13","port":6000}
{"ts":"2026-03-29T19:41:30Z","level":"error","msg":"ring exhausted","active_conns":4096}
```

---

## 2. Metrics — Latency Histograms

**Problem:** `lb_backend_avg_latency_ms` is a single average. One slow backend (or a single
very long-lived connection) is invisible in the average. The industry standard for latency
is percentile buckets.

**Standard:** Prometheus `histogram` type with buckets at p50, p95, p99, p99.9.
Every production proxy (Envoy, nginx, HAProxy, Traefik) exposes latency histograms.

**Implementation:**

- Add a `HistogramBuckets` helper in `BackendManager.hpp` — a fixed array of `(upper_bound,
  atomic<uint64_t> count)` pairs.
- Suggested buckets (ms): `1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000, +Inf`
- In `op_done()`, after computing `us`, atomically increment the matching bucket.
- Expose in `MetricsServer.hpp` as:

```text
lb_backend_connection_duration_ms_bucket{backend="...",le="50"} 1234
lb_backend_connection_duration_ms_bucket{backend="...",le="100"} 2345
...
lb_backend_connection_duration_ms_count{backend="..."} 5000
lb_backend_connection_duration_ms_sum{backend="..."} 250000
```

---

## 3. Metrics — Byte Counters

**Problem:** No visibility into data volume. Required for capacity planning, billing,
and detecting traffic anomalies (e.g. a single client sending 10× normal volume).

**Implementation:**

- Add `atomic<uint64_t> bytes_in` and `bytes_out` to `BackendServer`.
- In the FILL/DRAIN CQE handler, add `res` (bytes moved) to the appropriate counter.
  `c2b` direction → `bytes_in`, `b2c` direction → `bytes_out`.
- Expose as:

```text
lb_backend_bytes_in_total{backend="..."} 1234567
lb_backend_bytes_out_total{backend="..."} 987654
```

---

## 4. Metrics — Error Breakdown

**Problem:** `lb_errors_total` is a single counter. A spike in resets looks identical
to a spike in timeouts, which require completely different responses.

**Implementation:**

- Distinguish error types at the `begin_teardown` call site — `res` from the CQE carries
  the errno: `-ECONNRESET`, `-ETIMEDOUT`, `-EBADF` (our own teardown), `-EPIPE`.
- Add per-type counters: `lb_errors_total{type="reset"}`, `lb_errors_total{type="timeout"}`,
  `lb_errors_total{type="backend_unavailable"}`.

---

## 5. Scale — OS Tuning Required for >10k Connections

**Problem:** At 100,000 concurrent connections with 60-second hold time:

- Each connection uses 6 file descriptors (client fd, backend fd, 4 pipe ends)
- 100k × 6 = 600,000 fds — exceeds default container `RLIMIT_NOFILE` of 1,024
- Memory: 100k × 8KB (pipe buffers after `F_SETPIPE_SZ`) = 800MB

The LB architecture handles this — the container and OS do not without tuning.

**Required (from `specs.md` section 6):**

```bash
fs.file-max = 1000000
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.ip_local_port_range = 1024 65535
vm.nr_hugepages = 1024
RLIMIT_NOFILE = unlimited (ulimit -n unlimited in container)
```

**k8s implementation:**

- Add an `initContainer` to `lb-deployment.yaml` that applies `sysctl` tuning.
- Or configure `sysctls` directly in the pod spec (requires `allowedUnsafeSysctls` in the
  admission controller for network sysctls).
- Increase `CONCURRENCY_MAX` in `main.cpp` from `10000` to match the tuned fd limit.
- Increase pod memory limit from `512Mi` to `2Gi` for 100k connection target.

---

## 6. Advanced io_uring Features (from `specs.md`)

The spec calls for three io_uring optimizations not yet implemented:

### 6a. Buffer Rings (`IORING_SETUP_BUF_RING` + hugepages)

Map `vm.nr_hugepages` 2MB pages into a registered buffer ring. The kernel DMA-transfers
directly into pre-pinned memory, eliminating TLB misses on every packet receive.
Currently each splice call causes a TLB miss on the pipe buffer pages.

### 6b. Direct Descriptors (`IORING_OP_FIXED_FILE`)

Register file descriptors with the ring once (`io_uring_register_files`). Subsequent
SQEs reference them by index, eliminating the kernel fd-table lookup on every
`IORING_OP_SPLICE`. At 250k ops/second, this removes 250k atomic reference count
increments per second from the kernel's fd table.

### 6c. RCU DNS Resolution Loop

The spec requires the LB to watch the ingestion headless `Service` via CoreDNS
re-queries rather than relying on pod self-registration. Implement a background
thread that periodically resolves `ingestion.default.svc.cluster.local` (DNS A records
from the headless service = one IP per pod), diffs against the current backend list,
and adds/removes backends atomically via `BackendManager`. This eliminates the control
endpoint entirely and handles pod restarts transparently.

---

## 7. Production Deployment — Bare Metal + systemd

Running the LB as a k8s pod is correct for development. In production the LB must run
**outside the cluster** on dedicated bare metal or VM nodes. Inside k8s the LB competes
for CPU with the workloads it routes, is subject to eviction, and cannot use hugepages
or full io_uring capabilities.

**Files:** `deploy/systemd/`

```text
deploy/systemd/
  loadbalancer.service       # systemd unit with LimitNOFILE=1000000, LimitMEMLOCK=infinity,
                             # AmbientCapabilities=CAP_SYS_NICE for SQPOLL
  loadbalancer-sysctl.conf   # kernel tuning (fs.file-max, somaxconn, hugepages, port range)
  install.sh                 # copies binary, applies sysctl, enables + starts the unit
```

**To deploy on a bare metal Ubuntu node:**

```bash
# On the Lima VM or a real Ubuntu node, after building:
sudo ./deploy/systemd/install.sh ./loadbalancer/build/loadbalancer

# Follow logs:
journalctl -u loadbalancer -f
```

The service restarts automatically on failure (`Restart=on-failure`), drains connections
gracefully on stop (`TimeoutStopSec=30s`), and drops all capabilities except `CAP_SYS_NICE`.

---

## 8. Network Architecture — Calico/Cilium BGP Peering

**The problem with the current k8s-internal LB:**
Every packet flows through the CNI plugin (veth pairs + iptables/eBPF rules), adding
latency on every splice operation. The LB and ingestion pods compete for CPU on the
same nodes. kube-proxy NAT adds another translation layer.

### The solution: direct pod IP routing via BGP

Calico (or Cilium) can peer with the bare metal LB node over BGP. The k8s pod CIDR
(e.g. `10.244.0.0/16`) is announced into the LB node's routing table via FRRouting.
The LB connects **directly to pod IPs** — no NAT, no iptables, no CNI overhead.
This is the architecture used by Cloudflare, Meta, and Google for their L4 proxies.

```text
Bare metal LB node                    k8s cluster (Calico)
┌──────────────────┐    BGP peer    ┌──────────────────────┐
│ C++ LB (io_uring)│◄──────────────►│ Calico node          │
│                  │                │ announces             │
│ routing table:   │                │ 10.244.0.0/16        │
│  10.244.0.0/16   │                │                      │
│  via k8s_node_ip │                │ Ingestion pods:       │
└──────────────────┘                │  10.244.0.13:6000    │
        │                           │  10.244.0.15:6000    │
        └───── direct TCP ─────────►│  10.244.0.16:6000    │
                                    └──────────────────────┘
```

**Files:** `deploy/calico/`

```text
deploy/calico/
  bgp-peer.yaml    # Calico BGPPeer resource — tells Calico to peer with the LB node
  frr.conf         # FRRouting config for the LB node — accepts BGP, installs pod routes
```

**Setup steps:**

```bash
# 1. Install Calico with BGP enabled (not VXLAN-only)
#    https://docs.tigera.io/calico/latest/getting-started/kubernetes/self-managed-onprem/onpremises

# 2. On the LB node, install and configure FRRouting
apt install frr
sed -i 's/bgpd=no/bgpd=yes/' /etc/frr/daemons
# Edit deploy/calico/frr.conf — replace LB_NODE_IP and K8S_NODE_IP
cp deploy/calico/frr.conf /etc/frr/frr.conf
systemctl restart frr

# 3. Apply the BGP peer to the cluster
# Edit deploy/calico/bgp-peer.yaml — replace LB_NODE_IP
kubectl apply -f deploy/calico/bgp-peer.yaml

# 4. Verify pod routes are installed on the LB node
ip route show | grep 10.244
# Expected: 10.244.0.0/24 via <k8s_node_ip> dev eth0 proto bgp

# 5. Test direct pod connectivity from the LB node
curl http://10.244.0.13:6000
```

Once BGP is peering, pod IPs are reachable from the LB node. The ingestion pods register
their `POD_IP:6000` via the control endpoint as today — the LB just connects directly
instead of going through a Service.

---

## Summary Table

| Gap | Severity | Effort |
| --- | -------- | ------ |
| Structured logging (spdlog) | High — perf impact at scale | Low |
| Remove per-connection stdout | High — perf impact at scale | Trivial |
| Latency histograms (p95/p99) | Medium — observability | Medium |
| Byte counters | Low — capacity planning | Low |
| Error type breakdown | Medium — oncall usability | Low |
| OS fd tuning for >10k conns | High — hard ceiling today | Medium |
| systemd bare metal deploy | High — required for prod | Done ✓ |
| Calico BGP peering | High — eliminates CNI overhead | Medium |
| Buffer rings + hugepages | Low — optimization | High |
| Direct descriptors | Low — optimization | High |
| RCU DNS loop | Medium — operational simplicity | High |
