#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
env_file="${script_dir}/.env"
secrets_dir="${script_dir}/secrets"
check_docker=true

usage() {
    echo "Usage: $0 [--env-file FILE] [--secrets-dir DIR] [--skip-docker]" >&2
}

fail() {
    echo "Preflight failed: $1" >&2
    exit 1
}

env_value() {
    local key=$1
    local file=$2
    local line name value found=""
    while IFS= read -r line || [[ -n ${line} ]]; do
        line=${line%$'\r'}
        [[ -n ${line} && ${line:0:1} != "#" && ${line} == *"="* ]] ||
            continue
        name=${line%%=*}
        value=${line#*=}
        if [[ ${name} == "${key}" ]]; then
            found=${value}
        fi
    done <"${file}"
    printf '%s' "${found}"
}

file_mode() {
    local file=$1
    if stat -c '%a' "${file}" >/dev/null 2>&1; then
        stat -c '%a' "${file}"
    else
        stat -f '%Lp' "${file}"
    fi
}

is_ipv4() {
    local address=$1
    [[ ${address} =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]
}

while (($# > 0)); do
    case "$1" in
        --env-file)
            (($# >= 2)) || fail "--env-file requires a path"
            env_file=$2
            shift 2
            ;;
        --secrets-dir)
            (($# >= 2)) || fail "--secrets-dir requires a path"
            secrets_dir=$2
            shift 2
            ;;
        --skip-docker)
            check_docker=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            fail "unknown argument: $1"
            ;;
    esac
done

[[ -f ${env_file} ]] ||
    fail "missing ${env_file}; copy .env.example to .env and edit it"

edge_host=$(env_value NEXUS_EDGE_HOST "${env_file}")
tunnel_port=$(env_value NEXUS_TUNNEL_PORT "${env_file}")
public_port=$(env_value NEXUS_PUBLIC_PORT "${env_file}")
runtime_uid=$(env_value NEXUS_RUNTIME_UID "${env_file}")
runtime_gid=$(env_value NEXUS_RUNTIME_GID "${env_file}")

[[ -n ${edge_host} ]] || fail "NEXUS_EDGE_HOST is missing"
for value_name in tunnel_port public_port runtime_uid runtime_gid; do
    value=${!value_name}
    [[ ${value} =~ ^[0-9]+$ ]] || fail "${value_name} must be numeric"
done
((tunnel_port >= 1 && tunnel_port <= 65535)) ||
    fail "NEXUS_TUNNEL_PORT is outside 1-65535"
((public_port >= 1 && public_port <= 65535)) ||
    fail "NEXUS_PUBLIC_PORT is outside 1-65535"
((tunnel_port != public_port)) || fail "tunnel and public ports must differ"
((runtime_uid != 0 && runtime_gid != 0)) ||
    fail "the container must not run as root"
[[ ${runtime_uid} == "$(id -u)" && ${runtime_gid} == "$(id -g)" ]] ||
    fail "NEXUS_RUNTIME_UID/GID must match id -u and id -g on this VM"

certificate_file="${secrets_dir}/edge-cert.pem"
private_key_file="${secrets_dir}/edge-key.pem"
token_file="${secrets_dir}/auth-token"
for secret in "${certificate_file}" "${private_key_file}" "${token_file}"; do
    [[ -s ${secret} ]] || fail "missing or empty secret: ${secret}"
    [[ -r ${secret} ]] || fail "secret is not readable: ${secret}"
done

command -v openssl >/dev/null 2>&1 || fail "openssl is required"
openssl x509 -in "${certificate_file}" -noout >/dev/null 2>&1 ||
    fail "edge-cert.pem is not an X.509 certificate"
openssl x509 -in "${certificate_file}" -checkend 3600 -noout >/dev/null 2>&1 ||
    fail "edge certificate expires in less than one hour"
if is_ipv4 "${edge_host}"; then
    openssl x509 -in "${certificate_file}" -checkip "${edge_host}" \
        -noout >/dev/null 2>&1 ||
        fail "certificate SAN does not match NEXUS_EDGE_HOST IP"
else
    openssl x509 -in "${certificate_file}" -checkhost "${edge_host}" \
        -noout >/dev/null 2>&1 ||
        fail "certificate SAN does not match NEXUS_EDGE_HOST DNS name"
fi

certificate_public_key=$(openssl x509 -in "${certificate_file}" \
    -pubkey -noout 2>/dev/null)
private_public_key=$(openssl pkey -in "${private_key_file}" \
    -pubout 2>/dev/null) || fail "edge-key.pem is not a usable private key"
[[ ${certificate_public_key} == "${private_public_key}" ]] ||
    fail "certificate and private key do not match"

token_length=$(tr -d '\r\n' <"${token_file}" | wc -c | tr -d ' ')
((token_length >= 32 && token_length <= 256)) ||
    fail "authentication token must contain 32-256 bytes"

for private_file in "${private_key_file}" "${token_file}"; do
    mode=$(file_mode "${private_file}")
    (( (8#${mode} & 077) == 0 )) ||
        fail "${private_file} must not be accessible by group or others"
done

if [[ ${check_docker} == true ]]; then
    command -v docker >/dev/null 2>&1 || fail "docker is required"
    docker compose version >/dev/null 2>&1 ||
        fail "the Docker Compose plugin is required"
    docker info >/dev/null 2>&1 || fail "the Docker daemon is not running"
    docker compose --env-file "${env_file}" \
        -f "${script_dir}/compose.yaml" config -q ||
        fail "Docker Compose configuration is invalid"
fi

echo "Public edge preflight passed"
echo "Edge host: ${edge_host}"
echo "Tunnel port: ${tunnel_port}; public port: ${public_port}"
