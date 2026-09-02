# Multi-stage image that builds and packages the dispatcher with perf capabilities.
FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates libssl-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target thermal_simd -j$(nproc)

FROM debian:12-slim
LABEL org.opencontainers.image.source="https://github.com/SaridakisStamatisChristos/Thermal_SIMD_Dispatcher"
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgcc-s1 libstdc++6 libssl3 ca-certificates && \
    rm -rf /var/lib/apt/lists/* \
    && mkdir -p /etc/thermal-simd /usr/share/doc/thermal-simd-dispatcher
COPY --from=build /src/build/thermal_simd /usr/local/bin/thermal_simd
COPY config/controller_coeffs.json /etc/thermal-simd/controller_coeffs.json
COPY LICENSE NOTICE THIRD_PARTY_NOTICES.md /usr/share/doc/thermal-simd-dispatcher/
ENV TSD_LOG_LEVEL=info \
    TSD_PREDICTIVE_COEFF_PATH=/etc/thermal-simd/controller_coeffs.json

# Keep the executable as the immutable entrypoint. Orchestrators can replace
# CMD/args without accidentally retaining a forced --health-check mode.
ENTRYPOINT ["/usr/local/bin/thermal_simd"]
CMD ["--health-check"]
