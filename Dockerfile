FROM ubuntu:24.04

# Avoid interactive prompts during apt installs
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libz-dev \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/polymarket

# Copy source
COPY . .

# Build all targets (FetchContent pulls simdjson, spdlog, httplib, IXWebSocket)
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# Runtime data directory
RUN mkdir -p data logs

CMD ["./build/src/harvester/polymarket_harvester"]
