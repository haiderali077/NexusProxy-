# NexusProxy Security Model

## Scope

Phase 7 protects the persistent developer-client-to-edge tunnel. It does not
automatically add TLS to arbitrary public TCP connections or to the connection
between the developer client and the local service.

## Assets

- Application payloads forwarded through the tunnel.
- The edge TLS private key.
- The shared client authentication token.
- Availability of the edge, tunnel, and exposed local service.
- Separation between unauthenticated peers and active routing state.

## Trust boundaries

```text
public user -- plain application TCP --> edge
edge       -- TLS 1.3 + token auth ----> developer client
client     -- local/private TCP --------> local service
```

The internet between the edge and client is untrusted. The edge host, developer
host, configured CA, certificate files, token source, OpenSSL installation, and
deployment platform are trusted.

## Security controls

### Tunnel encryption and edge identity

- OpenSSL provides TLS 1.3; NexusProxy does not implement cryptography.
- The edge loads a certificate chain and matching private key from runtime file
  paths.
- The client requires a configured CA and verifies the certificate chain.
- The client verifies the expected DNS name or IP address.
- Invalid chains, wrong names, plaintext peers, and incomplete handshakes are
  rejected before the framed protocol runs.

### Client authentication

- After TLS, the client sends a 32–256 byte bearer token in an `AUTH` frame.
- The edge uses `CRYPTO_memcmp()` for equal-length token comparison.
- The edge sends only generic success/failure responses.
- A tunnel ID and public routing state are created only after authentication.
- Three failures within ten seconds trigger a two-second global cooldown.

### Secret handling

- Tokens come from exactly one of `NEXUS_AUTH_TOKEN` or
  `NEXUS_AUTH_TOKEN_FILE`.
- Mounted files are recommended for containers and Kubernetes.
- Token owners wipe their primary heap storage with `OPENSSL_cleanse()`.
- Authentication frames and errors never intentionally log token or key data.
- `.env`, private build output, and local learning files are ignored by Git.

## Configuration

The edge requires:

```text
NEXUS_TLS_CERT_FILE
NEXUS_TLS_KEY_FILE
NEXUS_AUTH_TOKEN or NEXUS_AUTH_TOKEN_FILE
```

The client requires:

```text
NEXUS_TLS_CA_FILE
NEXUS_TLS_SERVER_NAME (recommended; defaults to the connection address)
NEXUS_AUTH_TOKEN or NEXUS_AUTH_TOKEN_FILE
```

Generate a token with a cryptographically secure tool, for example:

```bash
openssl rand -base64 32
```

Store the output in a restricted file and mount the same value into both
processes. Do not put real credentials in `.env.example`, source control,
container images, command-line arguments, or issue/PR text.

Certificates must include the edge's actual DNS name or IP in the Subject
Alternative Name extension. Use a trusted certificate authority or a private CA
whose root is distributed securely to the client.

## Threats addressed

- Passive tunnel observers cannot read forwarded payloads or auth frames.
- Active man-in-the-middle endpoints without a trusted certificate/name fail
  client verification.
- Clients without the shared token cannot activate routing state.
- Malformed/oversized frames and bounded queues limit memory abuse.
- Heartbeats, deadlines, reconnect backoff, and shutdown bounds limit hangs and
  retry storms.
- Failed-token cooldown limits online guessing and repeated TLS/auth work.

## Known limitations

- Public TCP traffic is not automatically encrypted. HTTP users need HTTPS
  termination at a reverse proxy/ingress or a future NexusProxy feature.
- Local-service traffic is not automatically encrypted; protect non-loopback
  local networks separately.
- Authentication uses one shared bearer token, not per-client identities, mTLS,
  scopes, rotation, expiration, or revocation.
- The rate limiter is process-global. One attacker can briefly delay a legitimate
  client, and limits are not shared across multiple edge replicas.
- The single-tunnel edge performs one bounded TLS/auth handshake at a time. Many
  slow connections can still reduce availability.
- TLS does not protect compromised endpoints. Malware/root access can read
  process memory, environment variables, mounted files, and plaintext before
  encryption or after decryption.
- Memory cleansing is best effort; transient copies may remain in libraries,
  allocator storage, registers, crash dumps, swap, or kernel buffers.
- Traffic analysis still reveals endpoints, timing, connection duration, and
  approximate byte counts.
- There is no certificate/token hot reload, automated certificate issuance,
  security metrics, distributed audit log, or external secret-manager client.
- OpenSSL and the operating system must be patched by the deployer.
- Phase 8 multi-tunnel routing and tenant isolation are not implemented yet.

## Operational recommendations

- Run both processes as non-root users with minimal filesystem/network access.
- Restrict private-key and token files to the service account.
- Use a certificate manager and secret store rather than baking secrets into
  images.
- Disable core dumps where appropriate and protect swap/process inspection.
- Place the edge behind firewall and connection-rate controls.
- Monitor generic TLS/auth failures without logging candidate credentials.
- Rotate certificates and tokens by restarting both sides until hot reload is
  implemented.
- Re-run normal, sanitizer, and security integration tests after dependency
  updates.

## Reporting vulnerabilities

Do not open a public issue containing credentials, private keys, captured user
payloads, or exploit details. Contact the repository owner privately and include
only the minimum information needed to reproduce the problem safely.
