# AES67eleven gateway: headless AES67/NMOS node (player, matrix, 2022-7, PTP)
# no virtual soundcard - that part needs the native macOS/Linux install
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make && \
    rm -rf /var/lib/apt/lists/*
COPY . /src
RUN cmake -B /build -S /src -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /build -j"$(nproc)"

FROM debian:bookworm-slim
COPY --from=build /build/aes67vsc-gateway /usr/local/bin/aes67vsc-gateway
COPY webui /usr/local/share/aes67-vsc/webui

# config, uploaded audio files and logs live here (mount it)
ENV HOME=/data
VOLUME /data
WORKDIR /usr/local/share/aes67-vsc

# web ui 8180, nmos node api 8081; ptp 319/320 + rtp are multicast -
# the container must run with network_mode: host anyway
EXPOSE 8180 8081

ENTRYPOINT ["/usr/local/bin/aes67vsc-gateway", "/data/config.json"]
