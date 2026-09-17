#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_file="${1:-$repo_root/.env.simulation}"
if [[ ! -f "$env_file" ]]; then
	echo "simulation environment file not found: $env_file" >&2
	exit 1
fi

set -a
# shellcheck disable=SC1090
source "$env_file"
set +a

# A clean checkout only tracks the go:embed placeholder. Build the integrated
# release binary first so the Expo SPA is staged into the binary before the
# simulation runtime starts. The build script cleans generated web files after
# compilation, while the resulting binary keeps the embedded bundle.
runtime_binary="${SIGAP_SIMULATION_BINARY:-/tmp/sigap-simulation}"
SIGAP_RELEASE_OUTPUT="$runtime_binary" "$repo_root/scripts/build-release.sh"

exec "$runtime_binary"
