#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
compose_file="${script_dir}/compose.yaml"
env_file="${script_dir}/.env"

usage() {
    cat >&2 <<'EOF'
Usage: edge.sh COMMAND

Commands:
  validate  Check configuration, credentials, and Docker without starting
  start     Validate, build, and start the public edge
  restart   Validate, rebuild, and replace the running edge container
  stop      Stop and remove the edge container and its private network
  status    Show the current Compose service state
  logs      Follow the latest edge logs
EOF
}

fail() {
    echo "Error: $1" >&2
    exit 1
}

require_compose() {
    [[ -f ${env_file} ]] ||
        fail "missing ${env_file}; copy .env.example to .env and edit it"
    command -v docker >/dev/null 2>&1 || fail "docker is required"
    docker compose version >/dev/null 2>&1 ||
        fail "the Docker Compose plugin is required"
}

(($# == 1)) || {
    usage
    exit 2
}

# Keeping the full Compose command in one array prevents quoting bugs when a
# path contains spaces and gives every command the same configuration.
compose=(
    docker compose
    --env-file "${env_file}"
    -f "${compose_file}"
)

case "$1" in
    validate)
        "${script_dir}/preflight.sh"
        ;;
    start)
        "${script_dir}/preflight.sh"
        "${compose[@]}" up --detach --build --remove-orphans
        "${compose[@]}" ps
        ;;
    restart)
        "${script_dir}/preflight.sh"
        "${compose[@]}" up --detach --build --force-recreate --remove-orphans
        "${compose[@]}" ps
        ;;
    stop)
        require_compose
        "${compose[@]}" down --remove-orphans
        ;;
    status)
        require_compose
        "${compose[@]}" ps
        ;;
    logs)
        require_compose
        "${compose[@]}" logs --tail 100 --follow edge
        ;;
    -h|--help)
        usage
        ;;
    *)
        usage
        fail "unknown command: $1"
        ;;
esac
