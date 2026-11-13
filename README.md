# NexusProxy

NexusProxy is a C++20 reverse TCP tunnel. A client running beside a local
service maintains an encrypted, authenticated connection to a public edge. The
edge accepts public TCP connections and multiplexes them through that tunnel.

## Repository layout

```text
apps/                 nexus-edge and nexus-client entry points
include/nexus/        headers shared by the libraries and applications
src/protocol/         frame encoding and decoding
src/security/         TLS, secrets, tokens, and authentication limits
src/runtime/          sockets, relaying, retries, heartbeat, and shutdown
tests/unit/           focused C++ tests
tests/integration/    end-to-end Python tests
docs/                 protocol documentation
deploy/kubernetes/    Kubernetes manifests
```

The root `CMakeLists.txt` owns project-wide compiler settings. Each major
directory owns the targets or tests defined inside it.

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable paths remain:

```text
build/nexus-edge
build/nexus-client
```

See [SECURITY.md](SECURITY.md) before running either process and
[docs/protocol.md](docs/protocol.md) for the wire-protocol design. Deployment
instructions live in [deploy/kubernetes/README.md](deploy/kubernetes/README.md).
