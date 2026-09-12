#!/usr/bin/env bash
# Start a local 3-replica Certain cluster, issue writes, then stop.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -x "${CERTAIN_BIN_DIR:-}/server" ]]; then
  BIN="$CERTAIN_BIN_DIR"
elif [[ -x "$ROOT/build/bin/server" ]]; then
  BIN="$ROOT/build/bin"
elif [[ -x "$ROOT/../build/bin/server" ]]; then
  BIN="$ROOT/../build/bin"
else
  echo "server binary not found. Build first:"
  echo "  cd $ROOT && cmake -S . -B build && cmake --build build --target server client -j"
  exit 1
fi

RUN="${CERTAIN_RUN_DIR:-$ROOT/build/run}"
LOG_DIR="$RUN/logs"
PID_DIR="$RUN/pids"
CLIENT_DIR="$RUN/client"

mkdir -p "$RUN/node0" "$RUN/node1" "$RUN/node2" "$LOG_DIR" "$PID_DIR" "$CLIENT_DIR"

PIDS=()
cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  if [[ -d "$PID_DIR" ]]; then
    for f in "$PID_DIR"/*.pid; do
      [[ -f "$f" ]] || continue
      pid="$(cat "$f" || true)"
      if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
      fi
    done
  fi
}
trap cleanup EXIT

wait_port() {
  local port="$1"
  local i
  for i in $(seq 1 60); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "timeout waiting for 127.0.0.1:${port}"
  return 1
}

start_node() {
  local index="$1"
  local log="$LOG_DIR/node${index}.log"
  echo "starting node ${index} (paxos $((10066 + index)), rpc $((11066 + index)))"
  "$BIN/server" --index="$index" --data_dir="$RUN/node${index}" \
    >"$log" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  echo "$pid" >"$PID_DIR/node${index}.pid"
}

run_client() {
  (cd "$CLIENT_DIR" && "$BIN/client" "$@")
}

echo "BIN=$BIN"
echo "RUN=$RUN"

start_node 0
start_node 1
start_node 2

wait_port 11066
wait_port 11067
wait_port 11068
echo "all RPC ports are up, wait for inter-replica connections"
sleep 3

echo
echo "=== write entry 1 via node0 ==="
run_client --cmd=write --server_ip=127.0.0.1 --server_port=11066 \
  --entity_id=13131 --entry=1 --value=hello-from-node0

echo
echo "=== getstringstatus via node1 (should see current_entry 1) ==="
run_client --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11067 \
  --entity_id=13131

echo
echo "=== write entry 2 via node2 ==="
run_client --cmd=write --server_ip=127.0.0.1 --server_port=11068 \
  --entity_id=13131 --entry=2 --value=hello-from-node2

echo
echo "=== appendstring via node1 (Replay then write next entry) ==="
run_client --cmd=appendstring --server_ip=127.0.0.1 --server_port=11067 \
  --entity_id=13131 --value=hello-append

echo
echo "=== getstringstatus on all three nodes (same committed entry/crc) ==="
run_client --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11066 \
  --entity_id=13131
run_client --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11067 \
  --entity_id=13131
run_client --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11068 \
  --entity_id=13131

echo
echo "demo finished (servers will be stopped)"
