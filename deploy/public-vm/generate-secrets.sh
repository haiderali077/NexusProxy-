#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
output_dir="${script_dir}/secrets"
san_type=""
edge_host=""
valid_days=30

usage() {
    echo "Usage: $0 (--dns NAME | --ip ADDRESS) [--output-dir DIR] [--days N]" >&2
}

fail() {
    echo "Error: $1" >&2
    exit 1
}

valid_ipv4() {
    local address=$1
    local first second third fourth octet
    [[ ${address} =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
    IFS=. read -r first second third fourth <<<"${address}"
    for octet in "${first}" "${second}" "${third}" "${fourth}"; do
        ((10#${octet} <= 255)) || return 1
    done
}

while (($# > 0)); do
    case "$1" in
        --dns)
            (($# >= 2)) || fail "--dns requires a name"
            [[ -z ${san_type} ]] || fail "choose only one of --dns and --ip"
            san_type=DNS
            edge_host=$2
            shift 2
            ;;
        --ip)
            (($# >= 2)) || fail "--ip requires an address"
            [[ -z ${san_type} ]] || fail "choose only one of --dns and --ip"
            san_type=IP
            edge_host=$2
            shift 2
            ;;
        --output-dir)
            (($# >= 2)) || fail "--output-dir requires a directory"
            output_dir=$2
            shift 2
            ;;
        --days)
            (($# >= 2)) || fail "--days requires a number"
            valid_days=$2
            shift 2
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

[[ -n ${san_type} ]] || { usage; fail "provide --dns or --ip"; }
[[ ${valid_days} =~ ^[1-9][0-9]*$ ]] || fail "--days must be positive"
if [[ ${san_type} == DNS ]]; then
    [[ ${edge_host} =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]] ||
        fail "invalid DNS name"
else
    valid_ipv4 "${edge_host}" || fail "invalid IPv4 address"
fi
command -v openssl >/dev/null 2>&1 || fail "openssl is required"

mkdir -p "${output_dir}"
chmod 700 "${output_dir}"

certificate_file="${output_dir}/edge-cert.pem"
private_key_file="${output_dir}/edge-key.pem"
token_file="${output_dir}/auth-token"
for target in "${certificate_file}" "${private_key_file}" "${token_file}"; do
    [[ ! -e ${target} ]] || fail "refusing to overwrite ${target}"
done

temporary_dir=$(mktemp -d "${output_dir}/.generate.XXXXXX")
cleanup() {
    rm -f "${temporary_dir}/edge-cert.pem" \
        "${temporary_dir}/edge-key.pem" "${temporary_dir}/auth-token"
    rmdir "${temporary_dir}" 2>/dev/null || true
}
trap cleanup EXIT

umask 077
openssl req -x509 -newkey rsa:3072 -sha256 -nodes \
    -days "${valid_days}" \
    -subj "/CN=${edge_host}" \
    -addext "subjectAltName=${san_type}:${edge_host}" \
    -keyout "${temporary_dir}/edge-key.pem" \
    -out "${temporary_dir}/edge-cert.pem" >/dev/null 2>&1
openssl rand -base64 -out "${temporary_dir}/auth-token" 32

chmod 600 "${temporary_dir}/edge-key.pem" \
    "${temporary_dir}/auth-token"
chmod 644 "${temporary_dir}/edge-cert.pem"

mv "${temporary_dir}/edge-cert.pem" "${certificate_file}"
mv "${temporary_dir}/edge-key.pem" "${private_key_file}"
mv "${temporary_dir}/auth-token" "${token_file}"

echo "Created edge credentials in ${output_dir}"
echo "Certificate SAN: ${san_type}:${edge_host}"
echo "Copy edge-cert.pem and auth-token securely to the developer machine."
