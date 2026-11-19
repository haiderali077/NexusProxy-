# NexusProxy

[![CI](https://github.com/haiderali077/NexusProxy-/actions/workflows/ci.yml/badge.svg)](https://github.com/haiderali077/NexusProxy-/actions/workflows/ci.yml)

NexusProxy is a self-hosted reverse TCP tunnel built with C++20, POSIX sockets,
OpenSSL, Docker, and Kubernetes. It exposes a service running on a developer's
computer through a reachable edge server without requiring inbound access to
the developer's network.

The developer client opens one persistent, authenticated TLS connection to the
edge. Public connections accepted by the edge are assigned stream IDs and
multiplexed through that tunnel to the local service. This works with HTTP and
other TCP-based protocols because NexusProxy forwards application bytes without
interpreting them.

> **Current scope:** one edge process accepts one authenticated developer
> tunnel and multiplexes up to 64 simultaneous public connections through it.

## Architecture

```text
Public user
    |
    | application TCP (for example HTTP on port 8000)
    v
+---------------------------+
| nexus-edge                |
| Public Linux host         |
|                           |
| :8000 public listener     |
| :7000 tunnel listener     |
+-------------+-------------+
              |
              | TLS 1.3 + token authentication
              | framed, multiplexed TCP tunnel
              v
+-------------+-------------+
| nexus-client              |
| Developer machine         |
+-------------+-------------+
              |
              | local TCP
              v
       127.0.0.1:9000
       local application
```

The client initiates the connection to the edge. That direction matters:
developer machines are commonly behind NAT and can make outbound connections
even when arbitrary internet users cannot connect directly to them.

## Features

- **End-to-end TCP forwarding** between a public socket and a local service.
- **Connection multiplexing** for up to 64 concurrent public streams over one
  persistent developer tunnel.
- **Binary framing protocol** with versioning, stream IDs, control messages,
  network-byte-order fields, streaming decoding, and a 64 KiB payload limit.
- **Non-blocking socket engine** using `poll()` so one connection does not block
  the entire process.
- **Backpressure and bounded memory** using per-stream credit windows, bounded
  output queues, and controlled `POLLOUT` monitoring.
- **TLS 1.3 tunnel encryption** with certificate-chain and DNS/IP identity
  verification through OpenSSL.
- **Token authentication** before the edge creates public routing state, plus
  constant-time comparison and failed-authentication rate limiting.
- **Failure recovery** with heartbeat detection, connection deadlines,
  exponential reconnect backoff with jitter, and automatic client reconnect.
- **Graceful shutdown** for `SIGINT` and `SIGTERM`, including stream cleanup and
  bounded TLS output draining.
- **Deployment support** through a multi-stage Docker image, hardened Docker
  Compose configuration, and a single-replica Kubernetes manifest.
- **Automated verification** through unit, TLS security, integration,
  concurrency, large-transfer, reconnect, shutdown, limits, and deployment
  tooling tests on Linux and macOS CI.

## Tech stack

| Component | Technology |
| --- | --- |
| Networking | C++20, POSIX sockets, TCP, `poll()` |
| Security | OpenSSL 3, TLS 1.3, bearer-token authentication |
| Protocol | Custom binary frames with per-stream flow control |
| Build | CMake 3.20+ |
| Testing | CTest, C++ unit tests, Python integration tests, ASan, UBSan |
| Packaging | Multi-stage Docker build, Docker Compose |
| Orchestration | Kubernetes Deployment, Service, and Secret mount |
| CI | GitHub Actions on Ubuntu and macOS |

## Quick start: run everything locally

This demonstration runs the edge, client, and local application on one machine.
It verifies the complete encrypted tunnel before deploying the edge to another
host.

### Prerequisites

- macOS or Linux
- A C++20 compiler
- CMake 3.20 or newer
- OpenSSL 3 development files
- Python 3 for the test suite and example local service
- Bash for credential and deployment scripts

On macOS with Homebrew:

```bash
brew install cmake openssl@3
```

On Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev python3
```

### 1. Build NexusProxy

From the repository root:

```bash
cmake -S . -B build
cmake --build build
```

If CMake cannot locate Homebrew's OpenSSL installation, configure with:

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build
```

The two executables are created at:

```text
build/nexus-edge
build/nexus-client
```

### 2. Generate local TLS and authentication credentials

Use an empty output directory. The generator deliberately refuses to overwrite
existing credentials.

```bash
deploy/public-vm/generate-secrets.sh \
  --ip 127.0.0.1 \
  --output-dir /tmp/nexusproxy-local
```

The command creates:

| File | Used by | Purpose |
| --- | --- | --- |
| `edge-cert.pem` | Edge and client | Edge certificate and client trust root |
| `edge-key.pem` | Edge only | Private key proving the edge's identity |
| `auth-token` | Edge and client | Shared tunnel authentication secret |

### 3. Start a local service — terminal 1

Python's built-in HTTP server provides a simple application to expose:

```bash
python3 -m http.server 9000 --bind 127.0.0.1
```

Confirm it works directly at <http://127.0.0.1:9000> before starting the
tunnel.

### 4. Start the edge — terminal 2

From the repository root:

```bash
NEXUS_TLS_CERT_FILE=/tmp/nexusproxy-local/edge-cert.pem \
NEXUS_TLS_KEY_FILE=/tmp/nexusproxy-local/edge-key.pem \
NEXUS_AUTH_TOKEN_FILE=/tmp/nexusproxy-local/auth-token \
./build/nexus-edge 7000 8000
```

Port `7000` accepts the encrypted developer tunnel. Port `8000` accepts public
application connections.

### 5. Start the developer client — terminal 3

```bash
NEXUS_TLS_CA_FILE=/tmp/nexusproxy-local/edge-cert.pem \
NEXUS_TLS_SERVER_NAME=127.0.0.1 \
NEXUS_AUTH_TOKEN_FILE=/tmp/nexusproxy-local/auth-token \
./build/nexus-client 127.0.0.1 9000 127.0.0.1 7000
```

The positional arguments are:

```text
./build/nexus-client LOCAL_IPV4 LOCAL_PORT EDGE_IPV4 TUNNEL_PORT
```

For this example:

```text
127.0.0.1:9000  local Python application
127.0.0.1:7000  NexusProxy tunnel listener
```

### 6. Send traffic through NexusProxy — terminal 4

```bash
curl http://127.0.0.1:8000
```

You can also open <http://127.0.0.1:8000> in a browser. The response originates
from the service on port `9000` and travels through the NexusProxy tunnel.

Stop each foreground process with `Ctrl+C`.

## Expose an existing local application

NexusProxy can forward any TCP service listening on an IPv4 address. Start the
application normally, then provide its address and port to `nexus-client`.

For example, if a React/Vite application is running on `127.0.0.1:5173`:

```bash
NEXUS_TLS_CA_FILE=/tmp/nexusproxy-local/edge-cert.pem \
NEXUS_TLS_SERVER_NAME=127.0.0.1 \
NEXUS_AUTH_TOKEN_FILE=/tmp/nexusproxy-local/auth-token \
./build/nexus-client 127.0.0.1 5173 127.0.0.1 7000
```

Visitors still connect to the edge's public port, `8000` by default. Only the
client's local-port argument changes.

## Command-line reference

### Edge

```text
./build/nexus-edge [tunnel-port] [public-port]
```

| Argument | Default | Description |
| --- | ---: | --- |
| `tunnel-port` | `7000` | Accepts the encrypted developer tunnel |
| `public-port` | `8000` | Accepts public TCP connections to forward |

### Client

```text
./build/nexus-client [local-address] [local-port] [edge-address] [tunnel-port]
```

| Argument | Default | Description |
| --- | --- | --- |
| `local-address` | `127.0.0.1` | IPv4 address of the local service |
| `local-port` | `9000` | Port used by the local service |
| `edge-address` | `127.0.0.1` | IPv4 address of `nexus-edge` |
| `tunnel-port` | `7000` | Edge tunnel-listener port |

The current connection helper accepts IPv4 literals. If the edge certificate
uses a DNS identity, pass the edge's IPv4 address as `edge-address` and put the
certificate's DNS name in `NEXUS_TLS_SERVER_NAME`.

## Environment variables

| Variable | Process | Required | Description |
| --- | --- | --- | --- |
| `NEXUS_TLS_CERT_FILE` | Edge | Yes | PEM certificate chain presented by the edge |
| `NEXUS_TLS_KEY_FILE` | Edge | Yes | PEM private key matching the certificate |
| `NEXUS_TLS_CA_FILE` | Client | Yes | Trusted CA or self-signed edge certificate |
| `NEXUS_TLS_SERVER_NAME` | Client | Recommended | DNS name or IP expected in the certificate SAN; defaults to the edge address |
| `NEXUS_AUTH_TOKEN_FILE` | Both | Recommended | Path containing the shared 32–256 byte token |
| `NEXUS_AUTH_TOKEN` | Both | Alternative | Token value supplied directly through the environment |

Set exactly one of `NEXUS_AUTH_TOKEN_FILE` and `NEXUS_AUTH_TOKEN`. File-based
secrets are preferred because environment values are easier to expose through
process inspection, shell history, or container configuration.

## Run the test suite

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite contains 25 tests covering:

- protocol encoding, streaming decoding, malformed frames, and binary payloads;
- socket ownership, relay queues, flow control, retry, heartbeat, and shutdown;
- TLS configuration, certificate verification, authentication, and rate limits;
- single requests, concurrent streams, large transfers, reconnects, deadlines,
  logging, resource limits, and tunnel confidentiality; and
- public-VM credential generation and deployment validation.

### AddressSanitizer and UndefinedBehaviorSanitizer

```bash
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNEXUS_ENABLE_SANITIZERS=ON
cmake --build build-sanitized
ctest --test-dir build-sanitized --output-on-failure
```

## Docker Compose edge deployment

The provider-independent public-VM deployment runs only `nexus-edge` in Docker.
The local application and `nexus-client` remain on the developer machine.

On a Linux host with Docker Engine and the Compose plugin:

```bash
cp deploy/public-vm/.env.example deploy/public-vm/.env
```

Edit `.env` with the host's public IPv4 address, published ports, and the values
returned by `id -u` and `id -g`. Then generate credentials using that exact IP:

```bash
deploy/public-vm/generate-secrets.sh --ip PUBLIC_IPV4
deploy/public-vm/edge.sh validate
deploy/public-vm/edge.sh start
deploy/public-vm/edge.sh status
```

Lifecycle commands:

```bash
deploy/public-vm/edge.sh logs
deploy/public-vm/edge.sh restart
deploy/public-vm/edge.sh stop
```

The Compose service runs as a non-root UID/GID with a read-only filesystem,
dropped capabilities, disabled privilege escalation, bounded PID usage, and
rotated logs. Credentials are mounted at runtime from Git-ignored files rather
than copied into the image.

See the [complete public-VM runbook](deploy/public-vm/README.md) for firewall,
credential-transfer, client, validation, and troubleshooting instructions.

## Kubernetes deployment

Build the edge image with the name used by the manifest:

```bash
docker build -t nexusproxy:1.0.0 .
```

Create the runtime Secret, then apply the single-replica edge Deployment and
LoadBalancer Service:

```bash
kubectl create secret generic nexusproxy-edge-secrets \
  --from-file=tls.crt=/path/to/edge-cert.pem \
  --from-file=tls.key=/path/to/edge-key.pem \
  --from-file=auth-token=/path/to/auth-token

kubectl apply -f deploy/kubernetes/edge-deployment.yaml
```

The certificate SAN must match the DNS name or IP supplied through
`NEXUS_TLS_SERVER_NAME` on the client. See the
[Kubernetes deployment notes](deploy/kubernetes/README.md).

## How the tunnel works

1. `nexus-client` connects to the edge tunnel port.
2. The client completes TLS 1.3 and verifies the edge certificate and identity.
3. The client sends its authentication token inside the encrypted connection.
4. After authentication and registration, the edge accepts public connections.
5. Each public connection receives a unique nonzero stream ID.
6. `OPEN`, `DATA`, `WINDOW_UPDATE`, and `CLOSE` frames for different streams are
   interleaved over the same TLS connection.
7. The client opens one matching connection to the configured local service for
   each stream and forwards bytes in both directions.
8. Heartbeats detect an unresponsive tunnel; the client reconnects with bounded
   exponential backoff after a disconnect.

TCP is a byte stream, so one `recv()` call may contain part of one frame or
several frames. The stateful decoder buffers arbitrary chunks and emits only
complete, validated frames. Full protocol details are documented in
[docs/protocol.md](docs/protocol.md).

## Resource and connection limits

| Limit | Value |
| --- | ---: |
| Active developer tunnels per edge | 1 |
| Concurrent streams per tunnel | 64 |
| Maximum frame payload | 65,536 bytes |
| Per-stream flow-control window | 256 KiB per direction |
| Per-stream output queue | 256 KiB |
| Tunnel output queue | 4 MiB |
| Tunnel control-message reserve | 64 KiB |

These limits keep malformed input and slow receivers from causing unbounded
userspace memory growth.

## Security boundaries

```text
public user -- application TCP --> edge
edge       -- TLS 1.3 + token --> developer client
client     -- local TCP -------> local service
```

TLS protects the persistent edge-to-client tunnel. NexusProxy does not
automatically encrypt the public-user-to-edge connection or the client-to-local
service connection. If the exposed service uses HTTP, the public port is plain
HTTP unless TLS is terminated separately in front of the edge.

The current edge uses one shared bearer token and accepts one developer tunnel.
Do not expose private administration panels, credentials, or development tools
that assume every visitor is trusted.

Read [SECURITY.md](SECURITY.md) before exposing a service outside a controlled
environment.

## Troubleshooting

| Problem | Check |
| --- | --- |
| `Address already in use` | Stop the old edge process or choose unused tunnel/public ports. |
| `NEXUS_TLS_CA_FILE must name...` | Set the client CA path to the edge certificate or issuing CA. |
| TLS hostname/IP verification fails | Ensure `NEXUS_TLS_SERVER_NAME` exactly matches a certificate SAN. |
| Authentication fails | Confirm both processes read the same token and that it contains 32–256 bytes. |
| Edge rejects a public connection | Start and authenticate `nexus-client` before connecting to the public port. |
| Client cannot reach the local service | Test the local address and port directly on the client machine. |
| Client cannot reach a remote edge | Use an IPv4 literal and verify the tunnel port is open in every firewall. |
| Browser shows an unexpected or blank page | Open the local application directly first; NexusProxy forwards its bytes unchanged. |
| Docker validation fails | Run `docker info`, verify `.env`, and confirm credential paths and permissions. |

## Repository layout

```text
apps/                 nexus-edge and nexus-client entry points
include/nexus/        public headers for protocol, runtime, and security modules
src/protocol/         frame encoding and streaming decoding
src/runtime/          sockets, relay queues, flow control, retry, and shutdown
src/security/         TLS contexts, secret loading, tokens, and auth limits
tests/unit/           focused C++ tests
tests/integration/    end-to-end Python network and security tests
tests/deployment/     public-VM deployment tooling tests
docs/                 protocol and project documentation
deploy/public-vm/     hardened Docker Compose deployment and operator scripts
deploy/kubernetes/    single-replica Kubernetes edge deployment
```
