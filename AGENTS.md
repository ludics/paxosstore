# PaxosStore — Agent Onboarding

Read this first. Human-facing history is in [README.md](./README.md); Certain runbook is in [certain/README.md](./certain/README.md).

## What this repo is

WeChat/Tencent open-source **Paxos consensus** from VLDB 2017. Two independent libraries, not one binary:

| Tree | Model | When to touch it |
|------|--------|------------------|
| `certain/` | **PaxosLog + DB**: each `entity_id` is a log; `entry` is the next sequence number; Chosen → Plog → apply DB | Default work area. CMake + 3-replica example are here. |
| `paxoskv/` | **PaxosLog-as-value**: consensus state stored as the KV value | Separate CMake. Do not fold into Certain’s build. |

C++11. Public Certain API: `certain::Certain` in `certain/include/certain/certain.h`.

## Layout (Certain)

| Path | Role |
|------|------|
| `certain/include/certain/` | Public API: `Certain`, `Options`, `Route` / `Plog` / `Db`, `errors.h` |
| `certain/src/` | Workers + Multi-Paxos (`entity_helper`, `entry_state`, `wrapper`) |
| `certain/network/` | TCP + epoll between replicas |
| `certain/default/` | Demo plugins: 3-node route, RocksDB plog, in-memory DB, TinyRpc service |
| `certain/tiny_rpc/` | Minimal protobuf RPC |
| `certain/example/` | `server.cc` / `client.cc` |
| `certain/scripts/run_3node_demo.sh` | Local 3-replica write/status demo |
| `certain/third/` | **Git submodules** (source only until you build them) |
| `certain/patches/` | libco TLS patch (high Linux tid) |
| `certain/BUILD` | Internal Bazel leftover (`protoc` load commented out). Not the open-source build. |

PaxosKV top-level: `core/`, `kv/`, `memkv/`, `dbcomm/`, `comm/`, `msg_svr/`, `example/`, plus its own submodules (`protobuf`, `leveldb`, `libco`, `snappy`, `googletest`).

## Fresh clone: submodules are not enough by themselves

A normal `git clone` only records **gitlinks** (pinned commits). Directories under `certain/third/*` and `paxoskv/{protobuf,leveldb,...}` look empty until you fetch submodule content.

`git submodule` with no arguments only prints help. Use:

```bash
git clone --recurse-submodules <url>
# or after a plain clone:
git submodule update --init --recursive
```

`--recursive` matters: protobuf / gflags pull nested repos (`third_party/benchmark`, `gflags/doc`, …).

That command **downloads pinned source**, not latest upstream, and **does not compile** anything.

Then:

1. **Certain** — `cd certain && sh third/autobuild.sh` (configure/make protobuf, rocksdb, libco, gtest, gflags, glog, gperftools; applies `patches/libco-thread-local-env.patch`).
2. **System Snappy** — Certain RocksDB expects `libsnappy` (`/usr/local/lib` or distro). It is **not** a Certain submodule (PaxosKV vendors `paxoskv/snappy`).
3. **PaxosKV** — `cmake -S paxoskv -B paxoskv/build && cmake --build paxoskv/build -j` (uses its own tree).

`.gitmodules` has `ignore = dirty` on every submodule. Local edits inside `third/` (including the applied libco patch) **do not show** in parent `git status`. Persist libco via `certain/patches/`, not a submodule pointer bump, unless you intend to fork.

Listed modules (12):

| Path | Upstream (pinned) |
|------|-------------------|
| `certain/third/{libco,protobuf,rocksdb,googletest,gflags,glog,gperftools}` | Tencent/libco, google/protobuf ~3.4, facebook/rocksdb ~5.8, gtest 1.8, gflags, glog, gperftools |
| `paxoskv/{libco,protobuf,leveldb,snappy,googletest}` | Same idea; PaxosKV uses LevelDB not RocksDB |

Need GitHub (or a reachable mirror). `ossattrapi` appears in old comments and is **not** in this checkout.

## Build and outputs

```bash
# Certain
cd certain
sh third/autobuild.sh          # first machine only
cmake -S . -B build
cmake --build build -j
# wrappers: ./build.sh example | test | all    or    make example

# from repo root (Certain only)
cmake -S . -B build && cmake --build build -j
```

Put binaries only under `certain/build/` / `paxoskv/build/`:

- `certain/build/bin/{server,client,tests/*}`
- `certain/build/lib/libcertain.a`

Do not add in-tree `certain/server`, `certain/client`, `*_test`, or `**/build/`. Root `.gitignore` already covers these.

CMake notes:

- Default RelWithDebInfo **strips `-DNDEBUG`** so `assert(sock.Init())` still runs.
- Default **no tcmalloc** (`CERTAIN_ENABLE_TCMALLOC=OFF`). Static tcmalloc + libco syscall hooks crash during `.init` on modern glibc.
- C++11, `-faligned-new` on GCC ≥ 7.

## Certain runtime model (do not guess)

- One **entity** = one Paxos log. Writes must be `entry == max_chosen + 1`. First write: `--entry=1`, or `appendstring` (Replay then next entry).
- **`Read` is not KV get.** It checks the entry is still empty (no in-flight write). Chosen entry → `kRetCodeEntryNotMatch` (`-3006`). Use `getstringstatus` / `appendstring` for committed DB state.
- 3-replica ports: Paxos `10066–10068`, client RPC `11066–11068`, tools `12066–12068`. Each process needs its own `--data_dir` (RocksDB plog).
- Write path: TinyRpc `Write` → `Certain::Write` → `EntityHelper` Promise/Accept/broadcast → majority Chosen → `PlogWorker` → `DbWorker::Commit` → RPC `ret 0`.
- Demo: `certain/scripts/run_3node_demo.sh`.

## Tests

```bash
cd certain/build && ctest --output-on-failure
```

Work dirs: `certain/build/test-work/<name>/`.  
Flaky under load: `array_timer_test.LongTime`, `msg_channel_helper_test`. Re-run those two alone before treating as regressions.

## Working conventions

- Generate C++11 only.
- Prefer CMake / `certain/build/`; do not revive the old in-tree Makefile object rules.
- Do not commit `compile_commands.json`, `*.pb.cc` / `*.pb.h`, coverage files, or submodule build products (`*.a` under `third/`).
- `certain/BUILD` Bazel file is WeChat-monorepo shaped; ignore it unless the user asks for Bazel.
- Keep changes scoped. Do not “clean up” vendored submodule trees in the parent commit.

## First hour checklist

1. `git submodule update --init --recursive` — confirm `certain/third/protobuf/src` has files.
2. `cd certain && sh third/autobuild.sh` — confirm `third/rocksdb/librocksdb.a` exists; libco contains `g_pCoEnvPerThread`.
3. `cmake -S certain -B certain/build && cmake --build certain/build -j`
4. `ctest --test-dir certain/build --output-on-failure`
5. `certain/scripts/run_3node_demo.sh` — three nodes should report the same `current_entry` / crc.
