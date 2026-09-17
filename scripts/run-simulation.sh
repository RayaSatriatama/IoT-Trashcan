#!/usr/bin/env bash

set -euo pipefail

env_file="${1:-.env.simulation}"
if [[ ! -f "$env_file" ]]; then
	echo "simulation environment file not found: $env_file" >&2
	exit 1
fi

set -a
# shellcheck disable=SC1090
source "$env_file"
set +a

exec go run ./cmd/sigap
