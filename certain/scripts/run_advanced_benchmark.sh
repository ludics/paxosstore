#!/usr/bin/env bash
# run_advanced_benchmark.sh
# Performs detailed benchmark comparing Single-Entity vs Multi-Entity,
# scaling concurrency (1, 4, 8, 16, 32, 64 threads),
# and measuring Server (Node0, Node1, Node2) and Client CPU utilization via pidstat.

set -e

ROOT="/data/workspace/paxosstore/certain"
BIN="$ROOT/build/bin"
RUN="$ROOT/build/run"
PID_DIR="$RUN/pids"
LOG_DIR="$RUN/logs"
REPORT_DIR="$ROOT/build/benchmark_reports"

echo "=== [1/4] Preparing Clean 3-Node Cluster ==="
pkill -f "server --index=" 2>/dev/null || true
sleep 1
rm -rf "$RUN" "$REPORT_DIR"
mkdir -p "$RUN/node0" "$RUN/node1" "$RUN/node2" "$LOG_DIR" "$PID_DIR" "$RUN/client" "$REPORT_DIR"

SERVER_PIDS=()
for idx in 0 1 2; do
  "$BIN/server" --index="$idx" --data_dir="$RUN/node${idx}" >"$LOG_DIR/node${idx}.log" 2>&1 &
  SERVER_PIDS+=("$!")
  echo "$!" > "$PID_DIR/node${idx}.pid"
done

cleanup() {
  echo "Terminating benchmark cluster..."
  for pid in "${SERVER_PIDS[@]:-}"; do
    kill -9 "$pid" 2>/dev/null || true
  done
  pkill -f "server --index=" 2>/dev/null || true
}
trap cleanup EXIT

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
echo "Cluster is UP. Server PIDs: Node0=${SERVER_PIDS[0]}, Node1=${SERVER_PIDS[1]}, Node2=${SERVER_PIDS[2]}"

run_case() {
  local title="$1"
  local threads="$2"
  local req_per_thread="$3"
  local single_entity="$4"
  local base_entity="$5"
  local log_prefix="$6"

  echo -e "\n=========================================================================="
  echo ">>> [Running] $title <<<"
  echo "Threads: $threads | Req/Thread: $req_per_thread | Total: $((threads * req_per_thread)) | SingleEntity: $single_entity"
  echo "--------------------------------------------------------------------------"

  # Start pidstat CPU monitoring in background for the 3 servers
  local pidstat_file="$REPORT_DIR/${log_prefix}_cpu.txt"
  pidstat -u 1 -p "${SERVER_PIDS[0]},${SERVER_PIDS[1]},${SERVER_PIDS[2]}" > "$pidstat_file" 2>&1 &
  local pidstat_pid=$!

  # Run benchmark client
  "$BIN/benchmark_client" \
    --server_ip=127.0.0.1 \
    --server_port=11066 \
    --threads="$threads" \
    --requests_per_thread="$req_per_thread" \
    --single_entity="$single_entity" \
    --base_entity="$base_entity" \
    --cmd=appendstring \
    --value_size=128

  # Stop pidstat
  kill "$pidstat_pid" 2>/dev/null || true

  # Extract average CPU usage
  echo -e "\n--- Server CPU Utilization Breakdown (pidstat summary) ---"
  grep -E "Average:.*server" "$pidstat_file" || tail -n 5 "$pidstat_file"
  echo "=========================================================================="
}

# 1. Single Entity (1 Key) Benchmark: 1 thread, 500 writes
run_case "Case 1: Single-Key (1 Entity, 1 Client Thread)" 1 500 true 70001 "case1_single_key_1t"

# 2. Multi-Entity Benchmark: 4 Threads (4 distinct Entities)
run_case "Case 2: Multi-Key (4 Entities, 4 Client Threads)" 4 300 false 80000 "case2_multi_key_4t"

# 3. Multi-Entity Benchmark: 8 Threads (8 distinct Entities - original setting)
run_case "Case 3: Multi-Key (8 Entities, 8 Client Threads)" 8 300 false 81000 "case3_multi_key_8t"

# 4. Multi-Entity Benchmark: 16 Threads (16 distinct Entities)
run_case "Case 4: Multi-Key (16 Entities, 16 Client Threads)" 16 300 false 82000 "case4_multi_key_16t"

# 5. Multi-Entity Benchmark: 32 Threads (32 distinct Entities)
run_case "Case 5: Multi-Key (32 Entities, 32 Client Threads)" 32 300 false 83000 "case5_multi_key_32t"

# 6. Multi-Entity Benchmark: 64 Threads (64 distinct Entities)
run_case "Case 6: Multi-Key (64 Entities, 64 Client Threads)" 64 250 false 84000 "case6_multi_key_64t"

echo -e "\nAll advanced benchmark cases completed successfully!"
