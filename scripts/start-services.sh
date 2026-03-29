#!/usr/bin/env bash
# Local minikube dev script.
# Builds images directly into minikube's Docker daemon, installs KEDA, deploys.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${TAG:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo latest)}"

LB_IMAGE="projectz/loadbalancer:$TAG"
INGESTION_IMAGE="projectz/ingestion:$TAG"

log() { echo "[$(date +%H:%M:%S)] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

command -v minikube &>/dev/null || die "minikube not found"
command -v kubectl  &>/dev/null || die "kubectl not found"
minikube status | grep -q "Running" || die "minikube is not running — run: minikube start"

# Build images directly inside minikube — no registry or image load needed.
log "Pointing Docker at minikube daemon ..."
eval "$(minikube docker-env)"

log "Building $LB_IMAGE ..."
docker build -f "$ROOT/loadbalancer/Dockerfile" -t "$LB_IMAGE" "$ROOT/loadbalancer"

log "Building $INGESTION_IMAGE ..."
docker build -f "$ROOT/ingestion/Dockerfile" -t "$INGESTION_IMAGE" "$ROOT"

# Install KEDA if not already present.
if ! kubectl get namespace keda &>/dev/null; then
  log "Installing KEDA ..."
  kubectl apply -f https://github.com/kedacore/keda/releases/download/v2.13.0/keda-2.13.0.yaml
  kubectl rollout status deployment/keda-operator -n keda --timeout=120s
else
  log "KEDA already installed — skipping"
fi

# Apply manifests, substituting image tags.
log "Deploying to minikube ..."
for f in "$ROOT/k8s/"*.yaml; do
  sed \
    -e "s|projectz/loadbalancer:latest|$LB_IMAGE|g" \
    -e "s|projectz/ingestion:latest|$INGESTION_IMAGE|g" \
    "$f" | kubectl apply -f -
done

# Watch pods in the background while waiting for rollouts.
kubectl get pods -w &
WATCH_PID=$!

kubectl rollout status deployment/loadbalancer --timeout=120s
kubectl rollout status deployment/ingestion    --timeout=120s

kill "$WATCH_PID" 2>/dev/null; wait "$WATCH_PID" 2>/dev/null || true

log "Done."
kubectl get pods,svc

# Kill any previous port-forward and start a fresh one silently in the background.
pkill -f "kubectl port-forward svc/loadbalancer" 2>/dev/null || true
kubectl port-forward svc/loadbalancer 8080:8080 9090:9090 >/dev/null 2>&1 &
log "Port-forward running (PID $!)"
log "  Proxy:   localhost:8080"
log "  Metrics: http://localhost:9090/metrics"
log "  Metrics JSON: http://localhost:9090/metrics/json"
