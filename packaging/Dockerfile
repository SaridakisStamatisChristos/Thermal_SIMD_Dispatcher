# Multi-stage image that builds and packages the dispatcher with perf capabilities.
FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target thermal_simd -j$(nproc)

FROM debian:12-slim
LABEL org.opencontainers.image.source="https://example.com/thermal_simd_dispatcher"
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgcc-s1 libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/thermal_simd /usr/local/bin/thermal_simd
ENV TSD_LOG_LEVEL=info
ENTRYPOINT ["/usr/local/bin/thermal_simd", "--health-check"]
CMD []
