#!/usr/bin/env bash
# Manage 3-node certain cluster for experiment and benchmarking
set -euo pipefail

ROOT="/data/workspace/paxosstore/certain"
BIN="$ROOT/build/bin"
RUN="$ROOT/build/run"
PID_DIR="$RUN/pids"
LOG_DIR="$RUN/logs"

mkdir -p "$RUN/node0" "$RUN/node1" "$RUN/node2" "$PID_DIR" "$LOG_DIR" "$RUN/client"

start_cluster() {
  echo "[Manage] Starting 3-node cluster..."
  for idx in 0 1 2; do
    local pid_file="$PID_DIR/node${idx}.pid"
    if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
      echo "[Manage] Node $idx already running (PID $(cat "$pid_file"))"
      continue
    fi
    local log_file="$LOG_DIR/node${idx}.log"
    local data_dir="$RUN/node${idx}"
    nohup "$BIN/server" --index="$idx" --data_dir="$data_dir" </dev/null >"$log_file" 2>&1 &
    local pid=$!
    echo "$pid" > "$pid_file"
    echo "[Manage] Started node $idx (PID $pid, Paxos $((10066+idx)), RPC $((11066+idx)))"
  done

  # Wait for RPC ports to become ready
  for idx in 0 1 2; do
    local port=$((11066 + idx))
    local ok=0
    for i in $(seq 1 40); do
      if (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; then
        exec 3<&-
        exec 3>&-
        ok=1
        break
      fi
      sleep 0.2
    done
    if [[ $ok -eq 1 ]]; then
      echo "[Manage] Port $port is UP."
    else
      echo "[Manage] ERROR: Port $port failed to start!"
      exit 1
    fi
  done
  # Allow inter-replica connection setup
  sleep 2
  echo "[Manage] Cluster is fully ready!"
}

stop_cluster() {
  echo "[Manage] Stopping cluster..."
  for idx in 0 1 2; do
    local pid_file="$PID_DIR/node${idx}.pid"
    if [[ -f "$pid_file" ]]; then
      local pid
      pid="$(cat "$pid_file" || true)"
      if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        echo "[Manage] Sent SIGTERM to node $idx (PID $pid)"
      fi
      rm -f "$pid_file"
    fi
  done
  sleep 1
  # Force kill if still alive
  for idx in 0 1 2; do
    pkill -f "server --index=$idx" 2>/dev/null || true
  done
  echo "[Manage] Cluster stopped."
}

clean_data() {
  stop_cluster || true
  echo "[Manage] Cleaning data and logs..."
  rm -rf "$RUN/node0" "$RUN/node1" "$RUN/node2" "$LOG_DIR"/* "$RUN/client"/*
  mkdir -p "$RUN/node0" "$RUN/node1" "$RUN/node2" "$PID_DIR" "$LOG_DIR" "$RUN/client"
  echo "[Manage] Clean completed."
}

status_cluster() {
  for idx in 0 1 2; do
    local pid_file="$PID_DIR/node${idx}.pid"
    if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
      echo "Node $idx: RUNNING (PID $(cat "$pid_file"))"
    else
      echo "Node $idx: STOPPED"
    fi
  done
}

case "${1:-}" in
  start)
    start_cluster
    ;;
  stop)
    stop_cluster
    ;;
  restart)
    stop_cluster
    sleep 1
    start_cluster
    ;;
  clean)
    clean_data
    ;;
  status)
    status_cluster
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|clean|status}"
    exit 1
    ;;
esac
