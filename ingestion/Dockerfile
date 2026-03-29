FROM golang:1.25 AS builder

WORKDIR /app
COPY go.mod .
COPY ingestion/ingestion.go ingestion/

RUN go build -o /app/server ./ingestion/ingestion.go

# ── runtime ──────────────────────────────────────────────────────────────────
FROM gcr.io/distroless/base-debian12

COPY --from=builder /app/server /server

EXPOSE 6000
ENTRYPOINT ["/server"]
