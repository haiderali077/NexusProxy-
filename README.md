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
tests/deployment/     public-VM deployment tooling tests
docs/                 protocol documentation
deploy/public-vm/     single-VM Docker Compose deployment
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

## Share a local service through a public VM

The first practical internet deployment uses Docker Compose on one Linux VM.
The repository includes credential generation, configuration validation,
non-root container settings, bounded logs, lifecycle commands, and an
end-to-end operator runbook:

- [Public VM deployment](deploy/public-vm/README.md)
- [Security model and limitations](SECURITY.md)

This gives NexusProxy a stable public address while `nexus-client` and the
application remain on the developer machine. The VM deployment supports one
active developer tunnel and one public service; multi-tenant routing is future
work.

See [docs/protocol.md](docs/protocol.md) for the wire-protocol design. The
Kubernetes alternative is documented in
[deploy/kubernetes/README.md](deploy/kubernetes/README.md).
