# Certain

Certain is an asynchronous Multi-Paxos log library used as the consensus layer of PaxosStore (`PaxosLog + DB`). It can be driven asynchronously or through a blocking API (libco wait/notify).

Each **entity** is an independent Paxos log stream. A write appends the next **entry**; after a majority of acceptors choose that entry, it is persisted to **Plog** and then applied to **DB**.

## Layout

| Path | Role |
|------|------|
| `include/certain/` | Public API (`Certain`, `Options`, `Route` / `Plog` / `Db`) |
| `src/` | Paxos state machine and worker pipeline |
| `network/` | TCP + epoll messaging between replicas |
| `default/` | Demo implementations (3-node route, RocksDB plog, in-memory DB) |
| `tiny_rpc/` | Minimal protobuf RPC used by the example |
| `example/` | 3-replica server and client |
| `third/` | Vendored protobuf / RocksDB / libco / gtest / gflags / glog / gperftools |

## Build

Dependencies are built once by `third/autobuild.sh`. All Certain outputs go to an out-of-tree directory:

```
certain/build/bin/server
certain/build/bin/client
certain/build/bin/tests/*
certain/build/lib/libcertain.a
```

```bash
cd certain
sh third/autobuild.sh          # first time only
cmake -S . -B build
cmake --build build -j

# or
./build.sh example             # deps + server/client
./build.sh test                # build + ctest
make example                   # Makefile wraps CMake
```

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
```

PaxosKV has its own CMake under `paxoskv/` and is built separately.

## Unit tests

```bash
cd certain
cmake --build build -j
cd build && ctest --output-on-failure
# or: make run_all_tests
```

Each test runs in `build/test-work/<name>/` so RocksDB/log files stay out of the source tree.

`array_timer_test.LongTime` and `msg_channel_helper_test` are timing-sensitive and can flake under load; re-run them alone if CTest reports a failure.

## 3-replica example

Ports (defaults):

| Node | `--index` | Paxos | Client RPC | Tools RPC |
|------|-----------|-------|------------|-----------|
| 0 | 0 | `127.0.0.1:10066` | `11066` | `12066` |
| 1 | 1 | `127.0.0.1:10067` | `11067` | `12067` |
| 2 | 2 | `127.0.0.1:10068` | `11068` | `12068` |

Each replica must use its own `--data_dir` (Plog is a local RocksDB):

```bash
mkdir -p build/run/node0 build/run/node1 build/run/node2
./build/bin/server --index=0 --data_dir=build/run/node0
./build/bin/server --index=1 --data_dir=build/run/node1
./build/bin/server --index=2 --data_dir=build/run/node2
```

Writes are sequential per entity (`entry == max_chosen + 1`). The first write to a new entity should use `--entry=1`, or use `appendstring` which calls `Replay` first.

`Read` is not a key-value get: it checks that the given entry is still empty (no in-flight write). Use `getstringstatus` / `appendstring` to observe committed DB state.

```bash
./build/bin/client --cmd=write --server_port=11066 \
  --entity_id=13131 --entry=1 --value=hello

./build/bin/client --cmd=read --server_port=11067 \
  --entity_id=13131 --entry=1

./build/bin/client --cmd=appendstring --server_port=11068 \
  --entity_id=13131 --value=world

./build/bin/client --cmd=getstringstatus --server_port=11066 \
  --entity_id=13131
```

A one-shot start / write / stop helper:

```bash
./scripts/run_3node_demo.sh
```

### Write path

1. Client `TinyRpc Write` → serving node's `TinyServiceImpl`
2. `Certain::Write` enqueues a `ClientCmd` on the entity worker
3. `EntityHelper` runs Promise / Accept and broadcasts `PaxosMsg` on ports 10066–10068
4. Majority accept → entry is **Chosen**
5. `PlogWorker` persists the record; `DbWorker` commits application state
6. The blocked RPC returns `ret 0`
