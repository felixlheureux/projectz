FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    cmake g++ liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN mkdir -p build && cd build && cmake .. && make

# ── runtime ──────────────────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y liburing2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/loadbalancer /usr/local/bin/loadbalancer

EXPOSE 8080 9090 8081
ENTRYPOINT ["loadbalancer"]
