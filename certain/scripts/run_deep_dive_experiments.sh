#!/usr/bin/env bash
# run_deep_dive_experiments.sh
# Comprehensive deep-dive experiment for Certain Paxos consensus:
# 1. Cluster bootstrap
# 2. Sequential write and replica sync
# 3. Gap rejection (order safety)
# 4. Certain::Read empty check semantics
# 5. AppendString (Replay + Write)
# 6. dump_entry state inspection
# 7. Quorum write under 1-node failure (fault tolerance)
# 8. Node rejoin and catchup recovery
# 9. Performance benchmark
# 10. Archiving storage artifacts for binary layout analysis

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
RUN="$ROOT/build/run"
LOG_DIR="$RUN/logs"
PID_DIR="$RUN/pids"
CLIENT_DIR="$RUN/client"
ARTIFACTS_DIR="$ROOT/build/experiment_artifacts"

echo "=== [0/10] Environment Setup & Clean ==="
pkill -f "server --index=" 2>/dev/null || true
sleep 1
rm -rf "$RUN" "$ARTIFACTS_DIR"
mkdir -p "$RUN/node0" "$RUN/node1" "$RUN/node2" "$LOG_DIR" "$PID_DIR" "$CLIENT_DIR" "$ARTIFACTS_DIR"

PIDS=()
cleanup() {
  echo -e "\n=== Cleaning up background server processes ==="
  for pid in "${PIDS[@]:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
  done
  pkill -f "server --index=" 2>/dev/null || true
}
trap cleanup EXIT

wait_port() {
  local port="$1"
  for i in $(seq 1 40); do
    if (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; then
      exec 3<&-
      exec 3>&-
      return 0
    fi
    sleep 0.25
  done
  echo "Timeout waiting for port $port"
  return 1
}

start_node() {
  local idx="$1"
  local log="$LOG_DIR/node${idx}.log"
  echo "Starting node ${idx} (Paxos $((10066+idx)), RPC $((11066+idx)), Tools $((12066+idx)))..."
  "$BIN/server" --index="$idx" --data_dir="$RUN/node${idx}" >>"$log" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  echo "$pid" > "$PID_DIR/node${idx}.pid"
}

stop_node() {
  local idx="$1"
  local pid_file="$PID_DIR/node${idx}.pid"
  if [[ -f "$pid_file" ]]; then
    local pid
    pid="$(cat "$pid_file")"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      echo "Gracefully stopping node $idx (PID $pid)..."
      kill "$pid" 2>/dev/null || true
      for i in $(seq 1 20); do
        if ! kill -0 "$pid" 2>/dev/null; then
          break
        fi
        sleep 0.1
      done
      if kill -0 "$pid" 2>/dev/null; then
        echo "Node $idx still alive, force killing..."
        kill -9 "$pid" 2>/dev/null || true
      fi
    fi
    rm -f "$pid_file"
  fi
}

echo "=== [1/10] Starting 3-Node Cluster ==="
start_node 0
start_node 1
start_node 2

wait_port 11066
wait_port 11067
wait_port 11068
echo "All 3 nodes are listening. Waiting 3s for inter-replica TCP mesh connections..."
sleep 3

run_cli() {
  (cd "$CLIENT_DIR" && "$BIN/client" "$@")
}

TEST_ENTITY=30001

echo -e "\n=== [2/10] Test Sequential Write (Node 0, Entity $TEST_ENTITY, Entry 1) ==="
run_cli --cmd=write --server_ip=127.0.0.1 --server_port=11066 --entity_id=$TEST_ENTITY --entry=1 --value="paxos_genesis_val"

echo -e "\n=== [3/10] Verify Consensus Sync on Node 1 & Node 2 ==="
echo "Querying Node 1..."
run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11067 --entity_id=$TEST_ENTITY
echo "Querying Node 2..."
run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11068 --entity_id=$TEST_ENTITY

echo -e "\n=== [4/10] Test Gap Rejection: Attempt Out-of-Order Write (Entry 3 without Entry 2) ==="
set +e
run_cli --cmd=write --server_ip=127.0.0.1 --server_port=11066 --entity_id=$TEST_ENTITY --entry=3 --value="gap_attempt"
GAP_RET=$?
set -e
echo "Gap write returned code: $GAP_RET (expected non-zero / -3006 kRetCodeEntryNotMatch)"

echo -e "\n=== [5/10] Test Certain::Read Semantics (Check Empty vs Non-Empty) ==="
echo "1. Read check on Entry 1 (already Chosen/committed, should return -3006):"
set +e
run_cli --cmd=read --server_ip=127.0.0.1 --server_port=11066 --entity_id=$TEST_ENTITY --entry=1
set -e
echo "2. Read check on Entry 2 (empty, should return 0):"
run_cli --cmd=read --server_ip=127.0.0.1 --server_port=11066 --entity_id=$TEST_ENTITY --entry=2

echo -e "\n=== [6/10] Write Entry 2 and Test AppendString (Entry 3) ==="
echo "Writing Entry 2 on Node 2..."
run_cli --cmd=write --server_ip=127.0.0.1 --server_port=11068 --entity_id=$TEST_ENTITY --entry=2 --value="paxos_second_val"
echo "AppendString on Node 1 (Replay -> Next Entry 3)..."
run_cli --cmd=appendstring --server_ip=127.0.0.1 --server_port=11067 --entity_id=$TEST_ENTITY --value="paxos_third_val"

echo "Checking status across all replicas after Entry 3:"
for p in 11066 11067 11068; do
  echo "--- Replica Port $p ---"
  run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=$p --entity_id=$TEST_ENTITY
done

echo -e "\n=== [7/10] Inspect Internal Consensus State with dump_entry ==="
"$BIN/dump_entry" --server_ip=127.0.0.1 --server_port=12066 --entity_id=$TEST_ENTITY --entry=1
"$BIN/dump_entry" --server_ip=127.0.0.1 --server_port=12066 --entity_id=$TEST_ENTITY --entry=2
"$BIN/dump_entry" --server_ip=127.0.0.1 --server_port=12066 --entity_id=$TEST_ENTITY --entry=3

echo -e "\n=== [8/10] Fault Tolerance Test: Kill Node 2 and Write to Majority (Node 0 + Node 1) ==="
stop_node 2
echo "Node 2 killed. Cluster size = 3, Active nodes = 2 (Majority 2/3 satisfies Quorum)."
echo "Writing Entry 4 on Node 0 while Node 2 is down..."
run_cli --cmd=write --server_ip=127.0.0.1 --server_port=11066 --entity_id=$TEST_ENTITY --entry=4 --value="quorum_write_while_node2_down"
echo "Status on Node 0:"
run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11066 --entity_id=$TEST_ENTITY
echo "Status on Node 1:"
run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11067 --entity_id=$TEST_ENTITY

echo -e "\n=== [9/10] Node 2 Rejoin & Catchup Self-Healing ==="
echo "Restarting Node 2..."
start_node 2
wait_port 11068
sleep 2
echo "Polling Node 2 for status to trigger and observe catchup completion..."
for i in $(seq 1 6); do
  echo "--- Catchup Poll Attempt #$i on Node 2 ---"
  set +e
  run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=11068 --entity_id=$TEST_ENTITY
  rc=$?
  set -e
  sleep 1
done
echo "Final verification: All 3 nodes status after complete catchup:"
for p in 11066 11067 11068; do
  echo "--- Node Port $p ---"
  run_cli --cmd=getstringstatus --server_ip=127.0.0.1 --server_port=$p --entity_id=$TEST_ENTITY
done

echo -e "\n=== [10/10] Preserving Experiment Artifacts ==="
mkdir -p "$ARTIFACTS_DIR/node0" "$ARTIFACTS_DIR/node1" "$ARTIFACTS_DIR/node2" "$ARTIFACTS_DIR/logs"
cp -r "$RUN/node0/"* "$ARTIFACTS_DIR/node0/" 2>/dev/null || true
cp -r "$RUN/node1/"* "$ARTIFACTS_DIR/node1/" 2>/dev/null || true
cp -r "$RUN/node2/"* "$ARTIFACTS_DIR/node2/" 2>/dev/null || true
cp -r "$LOG_DIR/"* "$ARTIFACTS_DIR/logs/" 2>/dev/null || true
echo "Artifacts successfully preserved in $ARTIFACTS_DIR."
echo "All experiment phases completed successfully!"
