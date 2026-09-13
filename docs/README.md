# PaxosStore Certain 核心共识协议深度源码解析与实战验证

本项目文档库深入剖析微信核心分布式共识存储引擎 **PaxosStore** 的 Multi-Paxos 实现库 —— **`Certain`**。
解析结合理论经典论文、C++11 工业级源码、真实可执行实验、底层落盘二进制分析与集群压测数据。

---

## 📚 知识导航与文档目录

| 文档 | 核心主题 | 涵盖内容与深度亮点 |
| :--- | :--- | :--- |
| [**01. 架构全景与多线程流水线设计**](./01_ARCHITECTURE_OVERVIEW.md) | 总体设计与流水线 | 微信 Entity-based 业务模型、PaxosLog+DB 解耦、Worker 流水线分工、无锁环形队列池、libco 协程异步同步化包装 |
| [**02. Paxos 状态机源码深度剖析**](./02_PAXOS_STATE_MACHINE.md) | 共识核心与算法证明 | 对照 Leslie Lamport *Paxos Made Simple*；`EntryStateMachine` 7 大状态演化、提议编号生成算法数学证明、Pre-Auth 1-RTT 快速路径、大 Value `has_value_id_only` 优化 |
| [**03. Plog 与 DB 落盘存储结构深度解析**](./03_STORAGE_ENGINE_AND_PLOG.md) | 存储引擎与二进制布局 | RocksDB 24 字节大端序 `EntryKey` 内存对齐设计、`EntryRecord` Protobuf 序列化、使用 `inspect_plog` 逐字节 Dump 真实落盘 Hex、`DbImpl` 内存 Shard 与定期快照机制 |
| [**04. 读写链路、重放追齐与自愈**](./04_READ_WRITE_REPLAY_AND_CATCHUP.md) | 协议链路与容灾恢复 | `Certain::Write` 逐行时序、`Certain::Read` 读语义实质（空槽位检查 vs 脑裂防御）、`Replay` 追齐提交、`CatchupWorker` 流量受控追齐、Node 重启自愈四阶段实测跟踪 |
| [**05. 实验实测、验证指南与基准性能测试**](./05_BENCHMARK_AND_EXPERIMENTAL_VALIDATION.md) | 实战测试与性能评测 | 完整 3 节点集群搭建实战、严格连续写入与跳洞拒绝实验、多数派宕机容灾实测、`benchmark_client` 压测报告（QPS 1000+，P99 < 10ms） |

---

## 🔬 关联学术论文与经典文献
1. **PaxosStore 原始论文**:
   - *PaxosStore: High-availability Storage Made Practical in WeChat* (VLDB 2017)
   - 作者：Jianjun Zheng, Qian Lin, Jiatao Xu, Cheng Wei, Chuwei Zeng, Pingan Yang, Yunfan Zhang (Tencent Inc. & NUS)
   - 对应代码：`paxosstore-high-availability-storage-made-practical-in-1108u7ru05.pdf`
2. **Paxos 理论奠基**:
   - *Paxos Made Simple* (Leslie Lamport, 2001)
   - *The Part-Time Parliament* (Leslie Lamport, 1998)
3. **Multi-Paxos 工业实践**:
   - *Paxos Made Live - An Engineering Perspective* (Google, PODC 2007)

---

## 🛠️ 配套实验与工具链速览
在本次深度解析中，不仅阅读代码，还在真实 Linux 环境中编译运行了配套的测试与工具链：
- `certain/scripts/run_deep_dive_experiments.sh`: 一键执行 3 副本全套功能验证与容灾测试
- `certain/tools/inspect_plog.cc`: 直接读取解析 RocksDB 底层 SST/WAL 二进制数据，反序列化 Paxos 记录
- `certain/tools/benchmark_client.cc`: 高性能并发压力测试客户端，输出吞吐量与 P50/P90/P99 延迟分布
- `certain/tools/dump_entry.cc`: 在线探测 Paxos 副本内部运行态内存状态机镜像
