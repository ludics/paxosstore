# 05. 实验实测、深度性能压测与 CPU 资源剖析

本篇文档提供完整的实验复现指南、实测数据报告、**单 Key vs 多 Key 性能对比**、**服务端/客户端 CPU 资源占用剖析**，以及解释**为什么初始压测 QPS 看起来不高、如何正确打满系统瓶颈**的深度架构剖析。

---

## 1. 测试平台与软硬件基准环境

| 硬件 / 环境维度 | 具体配置与参数说明 |
| :--- | :--- |
| **操作系统** | Linux 6.6.92-34.1.tl4.x86_64 |
| **CPU 架构** | x86_64 物理多核服务器 |
| **编译标准与模式** | C++11 (GCC 支持 `-faligned-new`)，`RelWithDebInfo`（剥离 `-DNDEBUG` 保证断言运行） |
| **核心底层依赖** | Vendored RocksDB 5.8, Protobuf 3.4, Tencent libco, Snappy 压缩库 |
| **集群拓扑** | 单物理机部署 3 节点对等 Paxos 实例（Node 0: 10066/11066, Node 1: 10067/11067, Node 2: 10068/11068） |
| **服务端线程池** | 每个 Server 进程常驻 **130+ 线程**（48 MsgWorker + 48 EntityWorker + 16 PlogWorker + 16 DbWorker + 其它） |

---

## 2. 深入探讨：为什么初始测试 QPS 看起来不高（~1,000 QPS）？

在初次基准测试中，我们测得 8 线程写入的吞吐约为 1,002 QPS。很多初学者看到“微信高可用存储”可能会下意识认为 QPS 应该轻松上万，为什么这里显示为 1,000 QPS？

### 2.1 利特尔法则（Little's Law）与客户端并发饥饿

测试工具 `benchmark_client` 采用的是标准多线程阻塞 RPC 模型：
- 客户端启动了 8 个 `std::thread` 工作线程；
- 每个线程独占一个 TCP 连接，**以串行同步阻塞的方式**发起 RPC 调用：
  ```cpp
  for (int r = 0; r < requests; ++r) {
    client.AppendString(&controller, &req, &rsp, nullptr); // 同步阻塞等待网络回包！
  }
  ```
- **核心瓶颈在客户端**：在任意一个给定时刻，整个测试进程**最多只有 8 个在途请求（In-Flight Requests = 8）**！

根据排队论核心定理 **利特尔法则（Little's Law）**：
$$N = X \times R \iff \text{Concurrency} = \text{Throughput (QPS)} \times \text{Latency (s)}$$
$$\text{Throughput (QPS)} = \frac{\text{Concurrency}}{\text{Latency}}$$

在 3 节点分布式 Paxos 集群中，每次 `AppendString` 写操作经历的完整链路：
1. Client $\to$ Node 0 TinyRPC 接收与解包；
2. 进入 `EntityWorker` 计算提议号；
3. 进入 `PlogWorker` 执行 RocksDB WAL 写入与刷盘；
4. 跨网络向 Node 1 和 Node 2 广播半对称报文并接收多数派确认；
5. 达成 Chosen，二次写入 Plog；
6. 提交 `DbWorker` 状态机；
7. TinyRPC 回包 $\to$ Client 读取响应。

整个端到端网络+落盘耗时约为 **$7.8 \text{ ms} = 0.0078 \text{ s}$**。
因此，单客户端线程的物理极限吞吐为：
$$\text{QPS}_{\text{per\_thread}} = \frac{1}{0.0078 \text{ s}} \approx 128 \text{ req/s}$$
8 个客户端线程能产生的总请求上限**在数学上被死死限制在**：
$$X_{\max} = 8 \times 128 \approx 1,024 \text{ req/s}!$$

**结论**：1,000 QPS **根本不是服务端的性能上限，而是客户端仅开启 8 个同步阻塞线程时的发压上限**！此时服务端 130 多个工作线程在 90% 以上的时间里都在空闲等待客户端发来下一个数据包！

---

## 3. 单 Key（单 Entity）vs 多 Key（多 Entity）深度对照实验

这是分布式共识领域最核心的理论机制之一：**Paxos 对单个 Key 是否能并发写入？**

### 3.1 理论辨析：单 Entity 的顺序性物理锁死
- **单 Entity（单 Key）**：每个 `entity_id` 是一条独立的连续 Paxos Log，写入必须满足 `entry == max_chosen_entry + 1`。同一 Entity 的多次写入在算法上**必须串行化**！即使客户端开 100 个并发线程同时打向同一个 Entity，也必然会因版本冲突而产生大量的重试或等待。
  因此，**单 Entity 的最大写入吞吐严格受限于往返延迟（RTT + Disk IO）**：
  $$\text{QPS}_{\text{single\_entity}} \le \frac{1}{\text{Round-Trip Latency}}$$
- **多 Entity（多 Key）**：PaxosStore 之所以能承载微信千亿级访问，其关键正是 **Entity-based 水平扩展**。数以亿计的不同用户和群聊分布在不同的 `entity_id` 上，由 48 个 `EntityWorker` 和 16 个 `PlogWorker` 完全并行并发处理！

---

### 3.2 真实梯度压测实验（1 线程至 64 线程）与 CPU 占用分析

我们通过升级后的 `certain/scripts/run_advanced_benchmark.sh`，在真实机器上使用 `pidstat -u 1` 监控 Node 0、Node 1、Node 2 服务端进程以及客户端的 CPU 占用，测得了 6 组严谨的对照实验数据：

#### 梯度实测汇总数据表

| 测试场景 | Entity 模式 | 并发线程数 | 总请求数 | QPS 吞吐 (req/s) | 平均延迟 (ms) | P99 延迟 (ms) | Node 0 CPU (主节点) | Node 1/2 CPU (副本节点) | 瓶颈分析 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **Case 1** | **单 Entity (单 Key)** | **1 线程** | 500 | **112.46** | 8.891 ms | 10.81 ms | 224% | 220% / 222% | 触及单 Key 串行延迟天花板 ($\approx 1/8.89\text{ms}$) |
| **Case 2** | 多 Entity (多 Key) | 4 线程 | 1,200 | **477.36** | 8.369 ms | 10.02 ms | 225% | 210% / 209% | 客户端 4 线程同步发压瓶颈 |
| **Case 3** | 多 Entity (多 Key) | 8 线程 | 2,400 | **1,008.88** | 7.857 ms | 9.91 ms | 243% | 212% / 213% | 客户端 8 线程同步发压瓶颈（初测重现） |
| **Case 4** | 多 Entity (多 Key) | 16 线程 | 4,800 | **2,130.66** | 7.291 ms | 9.54 ms | 290% | 214% / 215% | 吞吐线性翻倍，延迟不升反降（批处理效应） |
| **Case 5** | 多 Entity (多 Key) | 32 线程 | 9,600 | **4,036.96** | 7.712 ms | 10.46 ms | 398% | 242% / 243% | **QPS 突破 4,000**，多核并发真正开始释放 |
| **Case 6** | 多 Entity (多 Key) | 64 线程 | 16,000 | **4,344.02** | 13.823 ms | 21.06 ms | **428%** | 244% / 246% | 单机 3 副本多进程竞争物理 CPU/磁盘 IO |

---

## 4. 服务端与客户端 CPU 资源开销微观解析

结合 `pidstat` 捕获的 CPU 细分监控数据，我们深入剖析资源开销的真实去向：

```
--- Server CPU Utilization Breakdown (Case 6: 64 线程, 4344 QPS) ---
Time          UID       PID    %usr  %system  %guest   %wait    %CPU   CPU  Command
05:22:54 PM  1001   3421083  153.00   275.00    0.00    0.00  428.00     9  server (Node 0)
05:22:54 PM  1001   3421084   78.00   166.00    0.00    0.00  244.00     3  server (Node 1)
05:22:54 PM  1001   3421085   82.00   164.00    0.00    0.00  246.00    30  server (Node 2)
```

### 4.1 为什么空载时 Server 也有 ~200% CPU？
观察 Case 1（仅 1 个客户端发压），Node 0 的 CPU 占用也达到了约 224%。
**源码原因剖析**：
- 每个 Server 启动了 48 个 `MsgWorker`、48 个 `EntityWorker`、16 个 `PlogWorker`、16 个 `DbWorker`；
- 在无锁流水线中，各 Worker 在事件队列中频繁检查 `PopByOneThread`；
- 如果队列为空，代码采用 `usleep(1000)` 或 `poll(nullptr, 0, 1)` 主动出让，130 个线程的定时轮询累计造成了基准的 CPU 时间片开销（属于典型的为极低延迟而牺牲少许空载 CPU 的轮询设计）。

### 4.2 为什么 `%system`（内核态）显著高于 `%usr`（用户态）？
在 Case 6 高压下，Node 0 的 428% CPU 中，**`%system` 占了 275%，`%usr` 仅占 153%**！
这是因为：
1. **单机回路网络栈损耗**：
   三台 Server 部署在同一台机器的三个端口上，每次 Paxos 广播都必须走 Linux 内核的 `loopback` 网络栈。每次数据流动都伴随着 `epoll_wait`、`writev`、TCP 状态机流转、ACK 确认以及内核 Socket Buffer 拷贝；
2. **多线程并发系统调用**：
   RocksDB 写 WAL（`write` / `fdatasync`）与线程调度引发频繁的内核态切换。
在跨物理机生产部署时，由于网络开销由多台独立网卡与内核平摊，单机的 `%system` 占比会有明显改善。

### 4.3 为什么 Case 4 (16 线程) 延迟比 Case 1 (1 线程) 还要低？
- Case 1 平均延迟：**8.891 ms**
- Case 4 平均延迟：**7.291 ms**（下降了 1.6 ms！）
**架构原因**：
当并发较低时，每个写入请求到达 `PlogWorker` 时队列长度只有 1，RocksDB 必须针对单个请求执行写磁盘。
当并发达到 16~32 时，`PlogWorker` 的 `max_plog_batch_size = 20` 批量组提交生效，多个请求被合并为一个 `WriteBatch` 顺序追加，**每次 fsync 的开销被平摊**，从而让端到端延迟不升反降！

---

## 5. 官方生产压测模型对比（`task_perf.cc`）

微信官方原厂是如何进行高压性能测试的？
在源码 `certain/example/task_perf.cc` 中：
- `DEFINE_int32(task_num, 10);`：10 个系统发压线程；
- `DEFINE_int32(routine_num, 200);`：每个线程启动 200 个 libco 协程；
- **全集群并发数高达 $10 \times 200 = 2,000$ 个并发在途请求！**
- 并且每个协程独立分配一个独一无二的 `entity_id`：
  ```cpp
  uint64_t entity_id = ((id_ + 1) * 10000000000ULL + rid * 10000000ULL);
  ```
只有在海量 Entity、极高客户端协程并发的条件下，PaxosStore 的多 Worker 流水线才能被 100% 填满，在生产集群中展现出数万到数十万级别的惊人并发共识能力！

---

## 6. 本篇实验复现步骤

读者可在本地随时重新执行该高级基准测试，观察完整的性能与 CPU 变化：

```bash
cd certain
# 运行单 Key 与多 Key 对照及 CPU 抓取实验
./scripts/run_advanced_benchmark.sh
```
该脚本将依次自动化测试 1、4、8、16、32、64 线程场景，并在控制台实时输出延迟指标与 `pidstat` 资源占用报告。
