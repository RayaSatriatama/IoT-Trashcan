#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
embed_dir="$repo_root/internal/webapp/dist"
expo_dir="$repo_root/dist"
release_output="${SIGAP_RELEASE_OUTPUT:-/tmp/sigap-release}"

cd "$repo_root"

cleanup() {
	find "$embed_dir" -mindepth 1 -maxdepth 1 ! -name 'placeholder.txt' -exec rm -rf {} + 2>/dev/null || true
	rm -rf "$expo_dir"
}
trap cleanup EXIT

npm ci
npm test -- --run
npm run typecheck
CI=1 npm run export:web

if [[ ! -f "$expo_dir/index.html" ]]; then
	echo "release build: Expo export did not produce $expo_dir/index.html" >&2
	exit 1
fi

mkdir -p "$embed_dir"
find "$embed_dir" -mindepth 1 -maxdepth 1 ! -name 'placeholder.txt' -exec rm -rf {} +
cp -R "$expo_dir"/. "$embed_dir"/

go test ./... -count=1
go vet ./...
go test -race ./... -count=1
go build -o "$release_output" ./cmd/sigap

echo "release binary: $release_output"
