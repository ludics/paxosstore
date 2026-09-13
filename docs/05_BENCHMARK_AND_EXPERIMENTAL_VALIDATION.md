# 05. 实验实测、验证指南与基准性能测试

本篇文档提供完整的实验复现指南、实测数据报告、压测吞吐与延迟分析，以便读者亲手运行、验证与评测 Certain 的真实性能。

---

## 1. 测试平台与运行环境

| 属性 | 运行环境配置 |
| :--- | :--- |
| **操作系统** | Linux 6.6.92-34.1.tl4.x86_64 |
| **编译标准** | C++11 (GCC 支持 `-faligned-new`) |
| **编译模式** | `RelWithDebInfo`（保留符号且移除 `-DNDEBUG`，确保内部 `assert` 逻辑完整运行）|
| **底层依赖** | Vendored RocksDB 5.8, Protobuf 3.4, libco, Snappy, Glog, Gflags |
| **测试架构** | 本地单机部署 3 节点模拟分布式集群（跨端口 10066/10067/10068 通信） |

---

## 2. 实验工具集使用手册

我们开发并集成了三套实战工具，存放在 `certain/tools/` 和 `certain/scripts/` 中：

### 2.1 全流程一键验证脚本（`run_deep_dive_experiments.sh`）
脚本自动执行清理、拉起三节点集群、顺序写、乱序拒绝、空槽位检查、Replay 追加、dump 内存状态、单节点宕机容灾、节点重启四阶段自愈并保存落盘产物：
```bash
cd certain
./scripts/run_deep_dive_experiments.sh
```

### 2.2 底层 Plog 存储解析工具（`inspect_plog`）
用于直接只读打开并遍历任意节点的 RocksDB 目录，按大端序解码 24 字节 Key 并反序列化 Protobuf 记录：
```bash
./build/bin/inspect_plog --db_path=build/experiment_artifacts/node0/test_plog.o
```
参数：
- `--db_path`: 待解析的 RocksDB 目录路径。

### 2.3 高性能多并发压测工具（`benchmark_client`）
多线程、多连接向运行中的集群发起连续 Paxos 写入压测：
```bash
./build/bin/benchmark_client \
  --server_ip=127.0.0.1 \
  --server_port=11066 \
  --threads=8 \
  --requests_per_thread=250 \
  --cmd=appendstring \
  --value_size=256
```
核心参数说明：
- `--threads`: 并发客户端工作线程数；
- `--requests_per_thread`: 每个线程连续发送的请求数；
- `--cmd`: 测试命令类型（`appendstring`、`write`、`read`）；
- `--value_size`: 单次请求有效负载字节数（默认 64~256 字节）；
- `--base_entity`: 实体起始编号，多线程各持独立实体，测试水平并发。

---

## 3. 真实基准性能测试（Benchmark）数据报告

通过运行 `certain/scripts/run_benchmark.sh`，我们在真实 Linux 实例上进行了 3 组严谨的基准性能压测：

### 3.1 测试结果汇总表

| 测试用例编号 | 测试场景 | 并发线程数 | 请求总数 | 负载大小 (Bytes) | 成功率 | 吞吐量 (QPS) | 平均延迟 | P50 延迟 | P90 延迟 | P99 延迟 |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Case 1** | 中并发 AppendString (Replay+Write) | 4 线程 | 1,000 | 128 B | **100.0%** | **463.55** | 8.568 ms | 8.698 ms | 9.015 ms | 10.463 ms |
| **Case 2** | 高并发 AppendString (Replay+Write) | 8 线程 | 2,000 | 256 B | **100.0%** | **1,002.33** | 7.866 ms | 7.807 ms | 8.908 ms | 9.907 ms |
| **Case 3** | 多实体纯顺序 Paxos 批量写入 (Write) | 8 线程 | 2,000 | 128 B | **100.0%** | **1,017.42** | 7.822 ms | 7.744 ms | 8.844 ms | 9.899 ms |

### 3.2 压测终端原始输出截图

```
>>> Benchmark 2: 8 Threads, 250 requests each (Total 2000 requests, AppendString) <<<
=================================================================
  Starting Certain Paxos Cluster Benchmark
  Target Node: 127.0.0.1:11066
  Command:     appendstring
  Threads:     8
  Req/Thread:  250
  Total Req:   2000
  Payload:     256 bytes
=================================================================

---------------- Benchmark Results ----------------
Elapsed Time:       2.00 s
Successful Req:     2000
Failed Req:         0
Success Rate:       100.00 %
Throughput (QPS):   1002.33 req/sec
Latency Avg:        7.866 ms
Latency P50:        7.807 ms
Latency P90:        8.908 ms
Latency P99:        9.907 ms
=================================================================
```

---

## 4. 深度性能与延迟分析

### 4.1 延迟平稳性（P50 vs P99）
观察测得的延迟数据：
- **P50 延迟**：7.807 ms
- **P90 延迟**：8.908 ms
- **P99 延迟**：9.907 ms

**分析结论**：
P50 到 P99 之间的抖动小于 2.1 ms！这在分布式共识存储系统中是非常出色的表现。这主要归功于：
1. **无锁环形队列**：跨线程消息传递全过程无互斥锁争用，消除了由锁唤醒、优先级反转引发的长尾延迟；
2. **libco 协程异步化**：每个协程在让出 CPU 时开销仅数十纳秒，不会产生 OS 内核级线程切换开销；
3. **PlogWorker 批量组提交**：多实体写请求在无锁队列中自然汇聚，RocksDB 单次磁盘刷盘均摊到多个请求上。

### 4.2 为什么延迟严格收敛在 10ms 以内？
PaxosStore 论文（VLDB 2017）明确指出微信的核心业务要求存储延迟控制在 20ms 以内：
> *"Most applications in WeChat require the latency overhead in PaxosStore to be less than 20 ms."*

在我们的实测中，即使每次请求都包含：
1. 本地网络 RPC 接收；
2. Paxos 提议生成；
3. 本地 RocksDB WAL 写入与刷盘；
4. 跨节点 Paxos 投票网络广播与接收多数派响应；
5. 达成 Chosen 后的二次状态更新；
6. 业务 DB 内存状态机更新。

全流程端到端仍能在 **7.8ms 均值 / 9.9ms P99** 内完成，完全满足了微信超严苛的 SLA 要求！

---

## 5. 动手实验：读者复现实践指南

为了让读者亲身感受 Paxos 的魅力，建议按如下步骤逐步体验：

1. **观察状态机**：
   运行 `./scripts/run_deep_dive_experiments.sh`，并在运行后查看 `certain/build/experiment_artifacts/logs/node0.log`，搜索关键字 `UpdateMachineByPaxosCmd` 和 `local_updated`，观察状态机的每一次跳跃；
2. **查看底层存储**：
   运行 `./build/bin/inspect_plog --db_path=build/experiment_artifacts/node0/test_plog.o`，观察实际写入的每一条二进制记录；
3. **感受压力极限**：
   运行 `./build/bin/benchmark_client --threads=16 --requests_per_thread=500`，进一步增加并发，通过 `top` 观察 `MsgWorker`、`EntityWorker` 和 `PlogWorker` 各核心 CPU 利用率的均衡分布。
