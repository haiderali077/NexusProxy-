# Public VM deployment

This deployment runs `nexus-edge` on a Linux VM with a public address. Your
local machine runs `nexus-client`, which makes an outbound TLS connection to
the VM and forwards the VM's public port to a service on your machine.

```text
phone/browser -> VM:8000 -> encrypted NexusProxy tunnel -> Mac -> local app
```

The VM does not need inbound access to your home network. The client initiates
the tunnel from the Mac, so normal home NAT does not need port forwarding.

## 1. Prepare the VM

Create a current Linux VM with a public IPv4 address. A DNS name pointing to
that address is recommended, but the IP itself also works. Install Git, OpenSSL,
Docker Engine, and the Docker Compose plugin using your Linux distribution's
or Docker's official instructions.

Confirm the tools and Docker daemon are available to your non-root account:

```bash
git --version
openssl version
docker version
docker compose version
docker info
```

If you just added your account to the `docker` group, sign out and back in
before continuing. Access to Docker is effectively administrative access to the
VM, so only grant it to trusted accounts.

## 2. Download NexusProxy

Run these commands on the VM:

```bash
git clone https://github.com/haiderali077/NexusProxy-.git
cd NexusProxy-
git switch main
```

When testing this feature branch before it is merged, switch to
`feat/public-edge-deployment` instead.

## 3. Configure the public address and ports

```bash
cp deploy/public-vm/.env.example deploy/public-vm/.env
id -u
id -g
```

Edit `deploy/public-vm/.env`:

- Set `NEXUS_EDGE_HOST` to the VM's DNS name or public IPv4 address.
- Set `NEXUS_RUNTIME_UID` and `NEXUS_RUNTIME_GID` to the two numbers printed by
  `id -u` and `id -g`.
- Keep ports `7000` and `8000` unless those ports conflict with another service.

The file is ignored by Git because it is machine-specific. It should contain
configuration only; credentials belong in the ignored `secrets` directory.

## 4. Generate credentials

For a DNS name:

```bash
deploy/public-vm/generate-secrets.sh --dns edge.example.com
```

For a public IPv4 address:

```bash
deploy/public-vm/generate-secrets.sh --ip 203.0.113.10
```

Use the same value here and in `NEXUS_EDGE_HOST`. The script creates:

- `edge-cert.pem`: the public certificate the client will trust;
- `edge-key.pem`: the VM's private TLS key, which must never leave the VM; and
- `auth-token`: the shared secret required to activate a tunnel.

The generated certificate is self-signed and intended for a controlled demo.
For a long-lived production service, use a managed certificate or private CA
and a real secret manager with rotation.

Run the complete configuration check before opening the service:

```bash
deploy/public-vm/edge.sh validate
```

## 5. Configure both firewalls

Cloud providers usually have a network firewall called a security group,
network security group, or firewall rule. Add inbound TCP rules for:

| Port | Purpose | Recommended source |
| --- | --- | --- |
| `22` | SSH administration | Your current public IP only |
| `7000` | Encrypted NexusProxy tunnel | Your current public IP when practical |
| `8000` | Shared local application | The people/networks that need access |

If the VM also runs a host firewall such as UFW, allow the same application
ports there. For example:

```bash
sudo ufw allow 7000/tcp
sudo ufw allow 8000/tcp
sudo ufw status
```

Do not disable the firewall to make troubleshooting easier. Narrow rules are
safer and make the intended exposure clear.

## 6. Start the edge

On the VM:

```bash
deploy/public-vm/edge.sh start
deploy/public-vm/edge.sh status
```

The first start builds the multi-stage Docker image. Compose then runs it as a
non-root user with a read-only filesystem, dropped Linux capabilities, bounded
logs, and automatic restart after a VM reboot or process failure.

## 7. Transfer only the client credentials

On the Mac, create a private directory and copy the certificate and token from
the VM. Replace the sample SSH host with the VM's real address:

```bash
mkdir -p "$HOME/.config/nexusproxy"
scp vm-user@edge.example.com:NexusProxy-/deploy/public-vm/secrets/edge-cert.pem \
  "$HOME/.config/nexusproxy/edge-cert.pem"
scp vm-user@edge.example.com:NexusProxy-/deploy/public-vm/secrets/auth-token \
  "$HOME/.config/nexusproxy/auth-token"
chmod 600 "$HOME/.config/nexusproxy/auth-token"
```

Never copy `edge-key.pem` to the client. The client needs the certificate to
verify the edge, not the private key that proves the edge's identity.

## 8. Run the local app and client

Start the local application first. For example, a Vite/React development server
usually listens on `127.0.0.1:5173`. In a second Mac terminal, build NexusProxy:

```bash
cmake -S . -B build
cmake --build build --target nexus-client
```

Then connect the local service to the VM. Replace `edge.example.com` with the
exact DNS name or IP used in the certificate:

```bash
NEXUS_TLS_CA_FILE="$HOME/.config/nexusproxy/edge-cert.pem" \
NEXUS_TLS_SERVER_NAME=edge.example.com \
NEXUS_AUTH_TOKEN_FILE="$HOME/.config/nexusproxy/auth-token" \
./build/nexus-client 127.0.0.1 5173 edge.example.com 7000
```

Keep both the local app and `nexus-client` running. A visitor can now open:

```text
http://edge.example.com:8000
```

To prove the VM is genuinely public, test from a phone with Wi-Fi disabled.
Using cellular data ensures the request is coming from another network.

## Security boundary

The persistent connection between `nexus-client` and `nexus-edge` is protected
by TLS and token authentication. The public connection to port `8000` is raw
application TCP. If the local app speaks HTTP, visitors use plain HTTP unless
you add HTTPS termination in front of the public port. Do not expose private
admin panels, development secrets, or services that assume every visitor is
trusted.

This release supports one developer tunnel and one exposed public port. It is a
working public demo deployment, not a multi-tenant ngrok replacement.

## Operations and troubleshooting

Run these commands from the repository root on the VM:

```bash
deploy/public-vm/edge.sh status
deploy/public-vm/edge.sh logs
deploy/public-vm/edge.sh restart
deploy/public-vm/edge.sh stop
```

Common failures:

- **Preflight reports a SAN mismatch:** regenerate the certificate using the
  exact DNS name or IP in `NEXUS_EDGE_HOST`.
- **The client cannot connect to port 7000:** check the cloud firewall, host
  firewall, VM public address, `edge.sh status`, and `edge.sh logs`.
- **Port 8000 refuses connections:** confirm the tunnel client is authenticated
  and both firewalls allow the public port.
- **The page is blank or returns an application error:** test the local URL
  directly on the Mac. NexusProxy forwards bytes; it cannot fix a local app
  that is not serving the requested content.
- **Docker reports permission denied:** restore Docker access for the deployment
  account, then sign out and back in rather than running the whole workflow as
  root.

To update after new code is merged:

```bash
git pull --ff-only
deploy/public-vm/edge.sh restart
```

Credential rotation currently requires generating a new set, securely copying
the new certificate and token to the Mac, and restarting both sides together.
