#!/usr/bin/env bash
set -e

ROOT="/data/workspace/paxosstore/certain"
BIN="$ROOT/build/bin"
RUN="$ROOT/build/run"
PID_DIR="$RUN/pids"
LOG_DIR="$RUN/logs"

echo "=== [Benchmark] Preparing Clean Cluster Environment ==="
pkill -f "server --index=" 2>/dev/null || true
sleep 1
rm -rf "$RUN"
mkdir -p "$RUN/node0" "$RUN/node1" "$RUN/node2" "$LOG_DIR" "$PID_DIR" "$RUN/client"

PIDS=()
cleanup() {
  echo "Shutting down cluster servers..."
  for pid in "${PIDS[@]:-}"; do
    kill -9 "$pid" 2>/dev/null || true
  done
  pkill -f "server --index=" 2>/dev/null || true
}
trap cleanup EXIT

for idx in 0 1 2; do
  "$BIN/server" --index="$idx" --data_dir="$RUN/node${idx}" >"$LOG_DIR/node${idx}.log" 2>&1 &
  PIDS+=("$!")
  echo "$!" > "$PID_DIR/node${idx}.pid"
done

for port in 11066 11067 11068; do
  for i in $(seq 1 40); do
    if (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; then
      exec 3<&-
      exec 3>&-
      break
    fi
    sleep 0.2
  done
done
sleep 3
echo "Cluster UP. Launching benchmark_client..."

echo -e "\n>>> Benchmark 1: 4 Threads, 250 requests each (Total 1000 requests, AppendString) <<<"
"$BIN/benchmark_client" --server_ip=127.0.0.1 --server_port=11066 --threads=4 --requests_per_thread=250 --cmd=appendstring --value_size=128

echo -e "\n>>> Benchmark 2: 8 Threads, 250 requests each (Total 2000 requests, AppendString) <<<"
"$BIN/benchmark_client" --server_ip=127.0.0.1 --server_port=11066 --threads=8 --requests_per_thread=250 --cmd=appendstring --value_size=256

echo -e "\n>>> Benchmark 3: Pure Sequential Write across Multiple Entities <<<"
"$BIN/benchmark_client" --server_ip=127.0.0.1 --server_port=11066 --threads=8 --requests_per_thread=250 --cmd=write --value_size=128 --base_entity=60000

echo "Benchmark completed successfully."
