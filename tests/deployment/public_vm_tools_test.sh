#!/usr/bin/env bash

set -euo pipefail

(($# == 1)) || {
    echo "Usage: $0 DEPLOYMENT_DIRECTORY" >&2
    exit 2
}

deploy_dir=$1
generator="${deploy_dir}/generate-secrets.sh"
preflight="${deploy_dir}/preflight.sh"

fail() {
    echo "Deployment tooling test failed: $1" >&2
    exit 1
}

file_mode() {
    local file=$1
    if stat -c '%a' "${file}" >/dev/null 2>&1; then
        stat -c '%a' "${file}"
    else
        stat -f '%Lp' "${file}"
    fi
}

test_root=$(mktemp -d "${TMPDIR:-/tmp}/nexus-deploy-test.XXXXXX")
secrets_dir="${test_root}/secrets"
env_file="${test_root}/edge.env"
bad_env_file="${test_root}/bad-edge.env"
cleanup() {
    rm -f "${secrets_dir}/edge-cert.pem" \
        "${secrets_dir}/edge-key.pem" "${secrets_dir}/auth-token" \
        "${test_root}/overwrite.log" "${test_root}/permission.log" \
        "${test_root}/host.log" "${env_file}" "${bad_env_file}"
    rmdir "${secrets_dir}" 2>/dev/null || true
    rmdir "${test_root}" 2>/dev/null || true
}
trap cleanup EXIT

"${generator}" --dns edge.example.test --output-dir "${secrets_dir}" --days 2

[[ $(file_mode "${secrets_dir}/edge-key.pem") == 600 ]] ||
    fail "private key permissions are not 600"
[[ $(file_mode "${secrets_dir}/auth-token") == 600 ]] ||
    fail "token permissions are not 600"
openssl x509 -in "${secrets_dir}/edge-cert.pem" \
    -checkhost edge.example.test -noout >/dev/null 2>&1 ||
    fail "generated certificate does not contain the requested DNS SAN"

if "${generator}" --dns edge.example.test --output-dir "${secrets_dir}" \
    >"${test_root}/overwrite.log" 2>&1; then
    fail "generator overwrote existing credentials"
fi

cat >"${env_file}" <<EOF
NEXUS_EDGE_HOST=edge.example.test
NEXUS_TUNNEL_PORT=7000
NEXUS_PUBLIC_PORT=8000
NEXUS_RUNTIME_UID=$(id -u)
NEXUS_RUNTIME_GID=$(id -g)
EOF

"${preflight}" --skip-docker --env-file "${env_file}" \
    --secrets-dir "${secrets_dir}"

# A readable token is still unsafe if another local account can read it.
chmod 644 "${secrets_dir}/auth-token"
if "${preflight}" --skip-docker --env-file "${env_file}" \
    --secrets-dir "${secrets_dir}" >"${test_root}/permission.log" 2>&1; then
    fail "preflight accepted an overly permissive token"
fi
chmod 600 "${secrets_dir}/auth-token"

cat >"${bad_env_file}" <<EOF
NEXUS_EDGE_HOST=wrong.example.test
NEXUS_TUNNEL_PORT=7000
NEXUS_PUBLIC_PORT=8000
NEXUS_RUNTIME_UID=$(id -u)
NEXUS_RUNTIME_GID=$(id -g)
EOF

if "${preflight}" --skip-docker --env-file "${bad_env_file}" \
    --secrets-dir "${secrets_dir}" >"${test_root}/host.log" 2>&1; then
    fail "preflight accepted a host not covered by the certificate"
fi

echo "Public VM deployment tooling tests passed"
