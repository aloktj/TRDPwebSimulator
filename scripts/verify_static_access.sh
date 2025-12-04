#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATIC_DIR="$ROOT_DIR/static"
PORT="${PORT:-8000}"
HOST="${HOST:-127.0.0.1}"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

python3 -m http.server "$PORT" --bind "$HOST" --directory "$STATIC_DIR" >/tmp/trdp_static_server.log 2>&1 &
SERVER_PID=$!

# Give the server a moment to start
sleep 2

curl -fsS "http://${HOST}:${PORT}/" >/tmp/trdp_static_probe.html

echo "Static site reachable on http://${HOST}:${PORT}/"
