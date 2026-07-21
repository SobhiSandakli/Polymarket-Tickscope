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

# Drop root: run as an unprivileged user that owns the app dir. The feed
# parsers process attacker-influenceable WebSocket data, so they should not
# run as root inside the container.
RUN useradd --system --create-home --uid 10001 polymarket \
    && chown -R polymarket:polymarket /opt/polymarket
USER polymarket

CMD ["./build/src/harvester/polymarket_harvester"]
