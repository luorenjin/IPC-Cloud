FROM docker.m.daocloud.io/library/golang:1.25 AS builder
WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -o /ipccloud ./cmd/ipccloud

FROM docker.m.daocloud.io/library/debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /ipccloud /app/ipccloud
EXPOSE 8080 5060/udp 5060/tcp
VOLUME /app/data
ENTRYPOINT ["/app/ipccloud"]
