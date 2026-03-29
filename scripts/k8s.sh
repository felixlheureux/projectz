#!/usr/bin/env bash
set -euo pipefail

# ─── helpers ─────────────────────────────────────────────────────────────────

die()   { echo "ERROR: $*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: ./k8s.sh <command> [args]

Commands:
  status                     show all pods and services
  logs   <service> [flags]   tail logs (all replicas)
  exec   <service> [cmd]     shell into the first pod (default: bash)
  restart <service>          rollout restart a deployment
  scale  <service> <n>       scale a deployment to n replicas
  keda                       show KEDA scaler status and current replica count
  forward                    start port-forward (localhost:8080 proxy, localhost:9090 metrics)
  delete                     delete all projectz resources

Services: lb, ingestion
EOF
  exit 1
}

# Resolve service name → deployment/selector label
resolve() {
  case "$1" in
    lb|loadbalancer) echo "loadbalancer" ;;
    ingestion)       echo "ingestion" ;;
    *) die "Unknown service '$1'. Valid: lb, ingestion" ;;
  esac
}

# Get the first running pod for a deployment
first_pod() {
  local deploy="$1"
  kubectl get pods \
    -l "app=$deploy" \
    --field-selector=status.phase=Running \
    -o jsonpath='{.items[0].metadata.name}' 2>/dev/null \
    || die "No running pod found for '$deploy'"
}

# ─── commands ────────────────────────────────────────────────────────────────

cmd_status() {
  kubectl get pods,svc
}

cmd_logs() {
  [[ $# -lt 1 ]] && usage
  local deploy; deploy="$(resolve "$1")"; shift
  # -l selector streams logs from all replicas at once
  kubectl logs -l "app=$deploy" --all-containers=true -f "$@"
}

cmd_exec() {
  [[ $# -lt 1 ]] && usage
  local deploy; deploy="$(resolve "$1")"; shift
  local pod; pod="$(first_pod "$deploy")"
  local cmd="${1:-bash}"
  echo "→ exec into $pod"
  kubectl exec -it "$pod" -- "$cmd"
}

cmd_restart() {
  [[ $# -lt 1 ]] && usage
  local deploy; deploy="$(resolve "$1")"
  kubectl rollout restart deployment/"$deploy"
  kubectl rollout status  deployment/"$deploy" --timeout=120s
}

cmd_scale() {
  [[ $# -lt 2 ]] && usage
  local deploy; deploy="$(resolve "$1")"
  local n="$2"
  kubectl scale deployment/"$deploy" --replicas="$n"
  echo "Scaled $deploy to $n replica(s)"
}

cmd_keda() {
  echo "=== ScaledObject ==="
  kubectl get scaledobject ingestion-scaler
  echo ""
  echo "=== ingestion replicas ==="
  kubectl get deployment ingestion -o wide
  echo ""
  echo "=== HPA managed by KEDA ==="
  kubectl get hpa keda-hpa-ingestion-scaler 2>/dev/null || echo "(no HPA yet — no load)"
}

cmd_forward() {
  pkill -f "kubectl port-forward svc/loadbalancer" 2>/dev/null || true
  kubectl port-forward svc/loadbalancer 8080:8080 9090:9090 >/dev/null 2>&1 &
  echo "Port-forward running (PID $!)"
  echo "  Proxy:   localhost:8080"
  echo "  Metrics: http://localhost:9090/metrics"
}

cmd_delete() {
  read -rp "Delete all projectz k8s resources? [y/N] " yn
  [[ "$yn" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 0; }
  kubectl delete -f "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/k8s/" --ignore-not-found
}

# ─── dispatch ────────────────────────────────────────────────────────────────

[[ $# -lt 1 ]] && usage

case "$1" in
  status)  cmd_status ;;
  logs)    shift; cmd_logs    "$@" ;;
  exec)    shift; cmd_exec    "$@" ;;
  restart) shift; cmd_restart "$@" ;;
  scale)   shift; cmd_scale   "$@" ;;
  keda)    cmd_keda ;;
  forward) cmd_forward ;;
  delete)  cmd_delete ;;
  *)       usage ;;
esac
