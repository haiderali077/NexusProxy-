# Kubernetes edge deployment

The manifest runs only the public `nexus-edge`. The developer machine runs
`nexus-client` and connects to the tunnel service on port 7000.

Create the required Secret before applying the manifest:

```bash
kubectl create secret generic nexusproxy-edge-secrets \
  --from-file=tls.crt=/path/to/edge-certificate.pem \
  --from-file=tls.key=/path/to/edge-private-key.pem \
  --from-file=auth-token=/path/to/nexus-auth-token

kubectl apply -f deploy/kubernetes/edge-deployment.yaml
```

The certificate must contain the edge's public DNS name or IP address in its
Subject Alternative Name. Give the client the matching CA certificate and the
same authentication token.
