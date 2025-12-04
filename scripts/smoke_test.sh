#!/usr/bin/env bash
set -euo pipefail

PORT=${PORT:-8848}
XML_PATH=${XML_PATH:-"$(dirname "$0")/../configs/default.xml"}
BINARY=${BINARY:-"./build/trdp_web_simulator"}
COMID_TX=${COMID_TX:-1001}

if [ ! -x "$BINARY" ]; then
  echo "Binary not found at $BINARY. Build the project first (e.g. cmake --build ./build)." >&2
  exit 1
fi

if [ ! -f "$XML_PATH" ]; then
  echo "XML config not found at $XML_PATH" >&2
  exit 1
fi

cleanup() {
  if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" && wait "$SERVER_PID" || true
  fi
}
trap cleanup EXIT

$BINARY --xml "$XML_PATH" --port "$PORT" >/tmp/trdp_smoke_server.log 2>&1 &
SERVER_PID=$!
sleep 2

echo "Listing telegrams via REST..."
curl -sf "http://localhost:${PORT}/api/config/telegrams" | jq .

WEBSOCKET_LOG=$(mktemp)
python3 - <<'PY' "$PORT" "$COMID_TX" "$WEBSOCKET_LOG" &
import asyncio, json, os, subprocess, sys, time

port = int(sys.argv[1])
comid = int(sys.argv[2])
log_path = sys.argv[3]

try:
    import websockets  # type: ignore
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet", "--user", "websockets"])
    import websockets  # type: ignore

async def main():
    uri = f"ws://localhost:{port}/ws/telegrams"
    async with websockets.connect(uri) as ws:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            msg = await asyncio.wait_for(ws.recv(), timeout=deadline - time.monotonic())
            with open(log_path, "a", encoding="utf-8") as fh:
                fh.write(msg + "\n")
            payload = json.loads(msg)
            if payload.get("type") == "tx" and payload.get("comId") == comid:
                return
        raise SystemExit("TX confirmation not observed on websocket")

asyncio.run(main())
PY
WS_PID=$!

sleep 2
echo "Sending TX telegram ${COMID_TX}..."
curl -sf -X POST "http://localhost:${PORT}/api/telegrams/${COMID_TX}/send" \
  -H 'Content-Type: application/json' \
  -d '{"counter":1,"flag":true,"temperature":21.5}' | jq .

if ! wait "$WS_PID"; then
  echo "WebSocket listener did not receive TX confirmation. Log follows:" >&2
  cat "$WEBSOCKET_LOG" >&2
  exit 1
fi

echo "WebSocket TX confirmation received (log at $WEBSOCKET_LOG):"
cat "$WEBSOCKET_LOG"
