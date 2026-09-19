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
| [**06. 高性能网络通信与 TinyRPC 架构**](./06_NETWORK_AND_TINY_RPC.md) | 网络 I/O 与 RPC 体系 | `epoll` 反应堆、`MsgChannel` 全双工信道、`WriteItemList` 分散写零拷贝、线程间无锁队列通信机制、`TinyRPC` 框架剖析与实测（30,000+ QPS / 0.13ms 纯网络延迟） |
| [**07. 半对称协议与端到端 Paxos 流程**](./07_SEMI_SYMMETRIC_PAXOS_AND_FLOW.md) | 协议报文与三节点时序 | 半对称报文（`PaxosMsg`）哲学、`SwitchToLocalView` 视角反转、A/B/C 三节点完整交互流转、Pre-Authorization（预授权租约）源码深挖 |
| [**08. 协议日志 PLog 的生命周期与 GC**](./08_PLOG_LIFECYCLE_AND_GC.md) | PLog 读写与垃圾回收 | 写入全流程与 RocksDB `WriteBatch` 组提交、`PlogReadonlyWorker` 读写分离、RocksDB `CompactionFilter`（`PlogFilter`）基于提交点安全物理回收 |
| [**09. 日志空洞、Catchup 追赶与 Recover**](./09_CATCHUP_AND_RECOVER_DEEP_DIVE.md) | 故障恢复与快照兜底 | 理论 Paxos 空洞 vs 为什么 Certain 客户端跳写报 `-3006`、双游标保证连续 Apply、`CatchupWorker` 双重令牌桶限流保护、旧 PLog 被清后的 `SnapshotRecover` 兜底恢复 |
| [**10. 源码深度研读路线与实操调试指南**](./10_STUDY_GUIDE_AND_DEBUG_TRICKS.md) | 进阶指引与调试秘籍 | 四阶段科学研读路径、核心函数必读清单、`dump_entry` 在线状态机捕获、`inspect_plog` 穿透存储检查、网络分区与故障注入实战实验 |
| [**11. 源码全景地图与核心文件索引**](./11_SOURCE_CODE_MAP_AND_INDEX.md) | 源码地图与索引清单 | Certain 全部子目录分层解析、核心类/结构体与核心方法逐一拆解、全局核心符号（Symbol）快速定位地图 |
| [**12. 详细源码文件索引地图（全量）**](./12_DETAILED_FILE_BY_FILE_INDEX.md) | 地毯式逐文件索引 | 对 `certain/` 下的每一个头文件、实现文件、测试文件与脚本逐一拆解：提供了什么、实现了什么 |
| [**13. `utils/` 通用组件原理**](./13_UTILS_COMPONENTS.md) | 底层原语与链表实测 | `LIGHTLIST` 侵入式链表 vs Linux `list_head`、时间轮 / LRU / MPSC 无锁队列 / 协程 Worker；附 `light_list_bench` 压测数字 |
| [**📊 现代协议时序矢量流程图**](./paxosstore-modern-protocol-process.svg) | 矢量时序图 (SVG) | 基于当前 C++11 代码库绘制的 Client、Wrapper、EntityWorker、PlogWorker、MsgWorker、Acceptor Peer 端到端调用流转 |
| [**📊 现代协议泳道密排图**](./paxosstore-modern-protocol-process-compact.svg) | 泳道流程图 (SVG) | 无 PreAuth 的 2-RTT 写入：七列泳道密排，对照当前 `HandleWriteCmd` / `HandlePaxosCmd` / `user_rsp_queue` |
| [**📊 PreAuth 1-RTT 泳道密排图**](./paxosstore-modern-protocol-process-preauth-compact.svg) | 泳道流程图 (SVG) | `IsLocalAcceptable` 成立时同一次 `HandleWriteCmd` 内 Promise+Accept，跳过 Phase 1 网络往返 |

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
- `certain/tools/light_list_bench.cc`: `LIGHTLIST` vs Linux `list_head` vs `std::list` 微基准（见 [13](./13_UTILS_COMPONENTS.md)）
