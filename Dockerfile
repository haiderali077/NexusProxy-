FROM gcc:14.2.0 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source

# Copy build metadata first so Docker can reuse dependency-related layers.
COPY CMakeLists.txt .
COPY apps/CMakeLists.txt apps/CMakeLists.txt
COPY src/CMakeLists.txt src/CMakeLists.txt
COPY include include
COPY apps apps
COPY src src

RUN cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target nexus-edge -j2

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /source/build/nexus-edge /usr/local/bin/nexus-edge

USER 10001:10001

EXPOSE 7000 8000

ENTRYPOINT ["nexus-edge"]
