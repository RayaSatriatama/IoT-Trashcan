#!/usr/bin/env bash
set -euo pipefail
umask 077
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
public_dir="${XDG_CACHE_HOME:-$HOME/.cache}/sigap-ble-manual"
cd "$root"
if [[ ! -x node_modules/.bin/esbuild ]]; then
  printf '%s\n' 'esbuild lokal tidak tersedia. Jalankan instalasi dependency project terlebih dahulu.' >&2
  exit 1
fi
mkdir -p "$public_dir"
node_modules/.bin/esbuild scripts/ble-manual/session.ts --bundle --format=esm --platform=browser --outfile="$public_dir/session.js"
install -m 600 scripts/ble-manual/index.html "$public_dir/index.html"
printf '%s\n' 'SIGAP manual BLE: http://localhost:8765' 'Server hanya bind 127.0.0.1. Tidak ada scan, pairing, flash, reset, atau probe otomatis.'
exec python -u -m http.server 8765 --bind 127.0.0.1 --directory "$public_dir"
