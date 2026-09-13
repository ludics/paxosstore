# 11. Certain 源码全景地图与核心文件索引

为了帮助开发者、架构师和分布式系统研究者系统化地查阅与研读 `certain` 源码，本文档提供了一份详尽的**代码地图索引（Source Code Map & Index）**。

本文档自顶向下覆盖了 `certain/` 下的每一个目录，并对核心源码文件的**设计职责、关键类/结构体、核心方法及上下游调用关系**进行了细致拆解。

---

## 1. 目录架构速查树

```
certain/
├── include/certain/         # [公共接口层] 业务对接的公共头文件 (Certain, Options, Db, Plog, Route 等)
├── src/                     # [核心共识层] Paxos 状态机、多线程流水线 Worker、实体调度与无锁队列
├── network/                 # [网络通信层] epoll 反应堆、全双工 MsgChannel、零拷贝 Scatter-Gather 发送链表
├── tiny_rpc/                # [RPC 框架层] 基于 libco 协程与 Google Protobuf 的高性能全双工 RPC 框架
├── proto/                   # [协议定义层] PaxosMsg 半对称协议、TinyRPC 服务、运维 Tools 协议定义
├── default/                 # [默认插件层] RocksDB PLog 引擎、1024 桶内存 DB、3 节点路由及默认业务实现
├── utils/                   # [基础工具层] 无锁环形队列、分级时间轮、令牌桶限流、自研线程与协程锁
├── example/                 # [样例与压测] 3 节点 Server、命令行 Client、原厂协程性能压测 task_perf
├── tools/                   # [运维与测试] inspect_plog (SST解析)、benchmark_client、dump_entry 等
├── scripts/                 # [自动化脚本] 集群运维、故障注入、梯度压测与数据采集脚本
└── patches/                 # [补丁集合] libco 线程局部变量 (TLS) 高 TID Linux 适配补丁
```

---

## 2. 模块分层与职责总览

| 模块目录 | 角色定位 | 核心技术要点 |
| :--- | :--- | :--- |
| **`include/certain/`** | 公共 API 抽象 | 面向应用方的纯虚插件接口（`Db`、`Plog`、`Route`）与静态 Facade 门面（`Certain`）。 |
| **`src/`** | 共识与管线中枢 | `EntryStateMachine` 状态机迁移、`EntityHelper` 调度中枢、48 线程 SEDA 无锁流水线。 |
| **`network/`** | 专用对等网络栈 | 基于 Linux `epoll` 的长连接池、8 字节 `MsgHeader` 封包、`writev` 分散写零拷贝。 |
| **`tiny_rpc/`** | 业务与运维 RPC | 基于 Tencent `libco` 协程的 64 并发轻量级 RPC，原生对接 Protobuf Reflection。 |
| **`proto/`** | 二进制消息定义 | 半对称协议 `PaxosMsg`（仅用一种报文统一所有 Phase）、`SnapshotRecover` 协议。 |
| **`default/`** | 开箱即用插件库 | 24 字节大端序 RocksDB `EntryKey` 编码、1024 桶 Sharded DB、增量 CRC 滚算。 |
| **`utils/`** | 底层高性能原语 | `LockFreeQueue` 原子无锁队列、`TrafficLimiter` 双重令牌桶、`ArrayTimer` $O(1)$ 时间轮。 |

---

## 3. 各目录与核心源码文件详析

### 3.1 公共接口层：`certain/include/certain/`

本目录对外暴露，不包含任何外部重型库（如 RocksDB、libco）的头文件依赖，保持接口极简。

| 文件名 | 核心类 / 接口 | 核心职责与关键方法 |
| :--- | :--- | :--- |
| **`certain.h`** | `class Certain` | **引擎顶级门面（Facade）**。提供静态方法：`Init()`、`Start()`、`Stop()`、`Write()`、`Read()`、`Replay()`、`Exist()`（去重检查）。所有调用均转发给单例 `Wrapper`。 |
| **`options.h`** | `class Options`<br>`class CmdOptions` | **全局与单请求配置**。管理线程数配置（`entity_worker_num`、`plog_worker_num` 等）、队列容量、超时时间（`client_cmd_timeout_msec`）、Pre-Auth 开关（`enable_pre_auth`）等。 |
| **`db.h`** | `class Db` | **业务状态机插件接口**。纯虚类，需业务方实现：`Commit(entity_id, entry, value)`（顺序写入业务库）、`GetStatus(entity_id, max_committed_entry, flag)`、`SnapshotRecover()`。 |
| **`plog.h`** | `class Plog` | **Paxos 日志存储插件接口**。纯虚类，需持久化引擎实现：`GetRecord()`、`SetRecord()`、`MultiSetRecords()`（批量写入）、`LoadMaxEntry()`（加载最大槽位）、`RangeGetRecord()`。 |
| **`route.h`** | `class Route` | **集群拓扑路由插件接口**。负责实体与副本映射：`GetLocalAddr()`、`GetLocalAcceptorId(entity_id, *id)`、`GetServerAddrId(entity_id, acceptor_id, *addr_id)`。 |
| **`errors.h`** | `enum RetCodeErrors`<br>`enum NetWorkErrors`<br>`enum ImplErrors` | **统一全局错误码**。包括核心状态码：`kRetCodeOk` (0)、`kRetCodeEntryNotMatch` (-3006, 槽位不匹配/跳洞)、`kRetCodeEntryUncertain` (-3007)、`kRetCodeCatchupPending` (-3023) 等。 |
| **`log.h`** | `class LogBase` | 抽象日志接口，解耦具体日志库实现。 |
| **`monitor.h`** | `class Monitor` | 监控上报抽象接口，统计 Paxos 各阶段耗时、队列溢出、Chosen 编号分布等。 |

---

### 3.2 核心共识与流水线层：`certain/src/`

Certain 的核心中枢，包含了 Multi-Paxos 状态机逻辑、SEDA 无锁队列流转与各类型工作线程。

#### 核心状态机与实体管理
| 文件名 | 核心类 / 结构体 | 核心职责与关键方法 |
| :--- | :--- | :--- |
| **`entry_state.h`<br>`entry_state.cc`** | `class EntryStateMachine` | **Paxos 状态机核心**。<br>1. 维护 7 种状态（`kNormal` $\to$ `kPromiseLocal` $\to$ `kMajorityPromise` $\to$ `kAcceptLocal` $\to$ `kChosen` 等）；<br>2. `Promise(bool pre_auth)`：利用代数公式生成全局唯一提案编号，处理 Pre-Auth 预留空间；<br>3. `Accept()`：严格执行 Lamport P2c 不变式，继承历史最大编号的值；<br>4. `Update()`：根据对端半对称报文合并本端状态；<br>5. `RestoreValueInRecord()`：大 Value `has_value_id_only` 优化恢复。 |
| **`entity_helper.h`<br>`entity_helper.cc`** | `class EntityHelper` | **单线程业务共识调度中枢**。<br>1. `HandleClientCmd()`：校验 `entry == max_chosen_entry + 1`，防范跳洞，驱动写/读；<br>2. `HandleWriteCmd()`：驱动 Promise $\to$ PLog 落盘 $\to$ 广播；<br>3. `HandleReadCmd()`：广播 `check_empty = true` 探针检查多数派空白；<br>4. `UpdateMachineByPaxosCmd()`：网络报文推进多数派判定；<br>5. `TryCatchup()`：空洞检测与追赶调度。 |
| **`entity_worker.h`<br>`entity_worker.cc`** | `class EntityWorker` | **实体工作线程宿主**。通过 `entity_id` 散列绑定，单线程独占一个 `EntityHelper`。按优先级无锁轮询：`user_req_queue_` $\to$ `user_rsp_queue_` $\to$ `entity_req_queue_` $\to$ `plog_rsp_queue_` $\to$ `recover_rsp_queue_`。 |
| **`entity_info_mng.h`<br>`entity_info_mng.cc`** | `class EntityInfoMng`<br>`class EntityInfoGroup` | 管理内存中活跃的 `EntityInfo`。`EntityInfoGroup` 提供 128 分片的读写锁保护表，供跨线程安全点查 `GetMaxChosenEntry`。 |
| **`entry_info_mng.h`<br>`entry_info_mng.cc`** | `class EntryInfoMng`<br>`struct EntryInfo` | 维护每个槽位的活跃信息（超时、未决标记、`waiting_msgs` 暂存槽、`machine` 智能指针），内置内存上限保护器 `MemoryLimiter`。 |
| **`command.h`<br>`command.cc`** | `class CmdBase`<br>`class ClientCmd`<br>`class PaxosCmd` | 进程内流水线流转的任务基类与子类。`PaxosCmd` 封装 Protobuf `PaxosMsg`，提供 `SwitchToLocalView` 视角反转方法。 |

#### 异步流水线与工作线程池
| 文件名 | 核心类 | 核心职责与关键方法 |
| :--- | :--- | :--- |
| **`async_queue_mng.h`<br>`async_queue_mng.cc`** | `class AsyncQueueMng` | **全系统无锁环形队列管线池**。统一管理 11 类队列，提供 `GetXxxQueueByEntityId(entity_id)`，实现严格的哈希无锁分区。 |
| **`wrapper.h`<br>`wrapper.cc`** | `class Wrapper` | **内部总控引擎单例**。管理所有 Worker 线程的生命周期；实现 `SyncWait` 协程调度桥梁；提供 `Replay` 线性化数据追齐循环。 |
| **`plog_worker.h`<br>`plog_worker.cc`** | `class PlogWorker`<br>`class PlogReadonlyWorker` | **PLog 异步批量刷盘线程**。<br>1. 从队列批量取出写请求；<br>2. 按 `HashId` 分组；<br>3. 调用 RocksDB `MultiSetRecords` 组提交；<br>4. `PlogReadonlyWorker` 处理只读与冷加载请求。 |
| **`db_worker.h`<br>`db_worker.cc`** | `class DbWorker` | **业务 DB 状态机提交线程**。严格校验 `cmd->entry() == max_committed_entry + 1`，线性化调用业务 `Db::Commit`。 |
| **`db_limited_worker.h`<br>`db_limited_worker.cc`**| `class DbLimitedWorker` | 当状态机遇到空洞（`entry > max_committed_entry + 1`）时，对乱序请求进行缓冲、排队与限流。 |
| **`conn_worker.h`<br>`conn_worker.cc`** | `class ConnWorker` | 独立监听 Paxos 对等互联端口（10066+idx），接收对端节点连接并交给 `ConnMng`。 |
| **`conn_mng.h`<br>`conn_mng.cc`** | `class ConnMng` | 跨线程连接分发池，将接收的 Socket 均衡投递到 48 个 `MsgWorker`。 |
| **`msg_worker.h`<br>`msg_worker.cc`** | `class MsgWorker` | **网络 I/O 线程**（默认 48 个）。基于 `Poller`（epoll）监听网络事件，从网络流解析出 `PaxosCmd` 并压入 `entity_req_queue_`。 |
| **`msg_channel_helper.h`<br>`msg_channel_helper.cc`**| `class MsgChannelHelper` | 管理 `MsgWorker` 旗下的所有 Channel 映射表与可写事件链表。 |
| **`msg_serialize.h`<br>`msg_serialize.cc`** | `class MsgSerialize` | 二进制封包辅助类，将 `PaxosCmd` 拼接 8 字节 `MsgHeader` 准备网络发送。 |
| **`catchup_worker.h`<br>`catchup_worker.cc`** | `class CatchupWorker` | **日志追赶线程**。内置 `TrafficLimiter` 令牌桶，对发出的重传报文进行字节速率和 QPS 双重限流。 |
| **`recover_worker.h`<br>`recover_worker.cc`** | `class RecoverWorker` | **快照恢复线程**。当 PLog 已被 GC 时，调用 `Db::SnapshotRecover` 进行全量状态机重置。 |
| **`tools_worker.h`<br>`tools_worker.cc`** | `class ToolsWorker` | 接收并处理运维调试指令（如在线抓取状态机镜像 `DumpEntry`）。 |
| **`libco_notify_helper.h`<br>`libco_notify_helper.cc`**| `class LibcoNotifyHelper` | 基于 Tencent `libco` 协程条件变量（`stCoCond_t`）实现同步 API 调用与异步唤醒。 |
| **`notify_helper.h`<br>`notify_helper.cc`** | `class NotifyHelper` | 基于 Linux `pipe` 或 `eventfd` 的传统线程通知降级方案。 |

---

### 3.3 网络通信层：`certain/network/`

自研轻量级网络库，专为 Paxos 对等节点之间的高吞吐、低延迟报文交换打造。

| 文件名 | 核心类 / 结构体 | 核心职责与关键方法 |
| :--- | :--- | :--- |
| **`tcp_socket.h`<br>`tcp_socket.cc`** | `class TcpSocket` | 非阻塞 TCP 套接字封装：`InitSocket`（设置 `O_NONBLOCK`、`TCP_NODELAY`）、`Connect`、`Listen`、`Accept` 等。 |
| **`poller.h`<br>`poller.cc`** | `class Poller` | Linux `epoll` 事件驱动反应堆模型封装：`AddFd`、`ModFd`、`DelFd`、`Poll(timeout_ms)`。 |
| **`fd_obj.h`** | `class FdObj` | 挂载于 epoll 的虚基类，规范 `HandleRead()` 与 `HandleWrite()` 回调。 |
| **`msg_channel.h`<br>`msg_channel.cc`** | `class MsgChannel` | **对等全双工网络信道**。继承 `FdObj`，管理接收缓冲区，处理报文粘包拆包，驱动写链表。 |
| **`msg_header.h`<br>`msg_header.cc`** | `struct MsgHeader` | **8 字节固定网络协议头**：魔数（`0xfe`，1 字节）、消息类型（1 字节）、头部长度（2 字节）、载荷长度（4 字节，大端序）。 |
| **`write_item_list.h`<br>`write_item_list.cc`** | `class WriteItemList`<br>`struct WriteItem` | **高性能零拷贝分散写链表**。将多个待发送的离散内存块打包为 `iovec` 数组，一次性通过 `writev` 发送，避免内存大块拼接。 |
| **`inet_addr.h`** | `class InetAddr` | IP 地址与端口封装，支持与 64 位无符号整数相互转换（`uint64_t addr_id`）。 |

---

### 3.4 协议定义层：`certain/proto/`

系统全部采用 Google Protocol Buffers（proto2）定义跨网络和落盘结构。

| 文件名 | 核心 Message / Service | 协议语义与用途 |
| :--- | :--- | :--- |
| **`certain.proto`** | `message EntryRecord`<br>`message PaxosMsg`<br>`message RangeCatchupMsg` | **核心共识协议定义**。<br>1. `EntryRecord`：包含 `prepared_num`、`promised_num`、`accepted_num`、`value`、`chosen` 等字段，既是落盘格式又是报文载荷；<br>2. `PaxosMsg`：**半对称协议报文**，同时容纳发送方视角的 `local_entry_record` 和对端视角的 `peer_entry_record`。 |
| **`tiny_rpc.proto`** | `service TinyRpcService`<br>`message WriteReq/Rsp`<br>`message ReadReq/Rsp`<br>`message AppendStringReq/Rsp`<br>`message SnapshotRecoverReq/Rsp` | **客户端与快照 RPC 服务定义**。对外暴露业务读写方法及节点间全量 DB 快照同步接口。 |
| **`tools.proto`** | `service ToolsService`<br>`message DumpEntryReq/Rsp` | 运维工具服务定义，支持在线按 `(entity_id, entry)` 抓取并打印运行态内存镜像。 |

---

### 3.5 RPC 框架层：`certain/tiny_rpc/`

Certain 内置的微型 RPC 框架，天然与 Tencent `libco` 协程生态深度整合。

| 文件名 | 核心类 | 核心职责与关键方法 |
| :--- | :--- | :--- |
| **`tiny_server.h`<br>`tiny_server.cc`** | `class TinyServer` | **协程并发 RPC 服务端**。继承自 `RoutineWorker<TcpSocket>`，内部启动 64 个 libco 协程并发轮询。使用 Protobuf 反射机制（`CallMethod`）动态路由请求。 |
| **`tiny_client.h`<br>`tiny_client.cc`** | `class TinyClient`<br>`class TinyChannel`<br>`class TinyController` | **RPC 客户端桩（Stub）**。继承自 `google::protobuf::RpcChannel`，在当前线程/协程中同步阻塞发起网络调用并解析回包。 |
| **`tiny_rpc.h`<br>`tiny_rpc.cc`** | `class TinyRpc` | 底层静态工具类，提供二进制报文帧读取（`ReceiveHeader`、`ReceiveBody`）与发送（`SendMessage`）。 |

---

### 3.6 默认插件实现层：`certain/default/`

包含系统可直接编译运行的默认存储、路由及业务 DB 实现。

| 文件名 | 核心类 | 核心职责与关键方法 |
| :--- | :--- | :--- |
| **`plog_impl.h`<br>`plog_impl.cc`** | `class PlogImpl`<br>`class EntryKey` | **基于 RocksDB 的 PLog 引擎实现**。<br>1. 紧凑的 24 字节大端序 `EntryKey(entity_id, entry, value_id)`；<br>2. `LoadMaxEntry`：利用大端序和 Seek 哨兵向前一步 `iter->Prev()` 实现 $O(\log N)$ 寻址；<br>3. `MultiSetRecords`：RocksDB `WriteBatch` 组提交落盘。 |
| **`db_impl.h`<br>`db_impl.cc`** | `class DbImpl`<br>`class DbInfo`<br>`class DbDumper` | **1024 桶内存哈希业务 DB**。<br>1. 每个桶独立读写锁；<br>2. 记录 `(entry, crc32, flag)`，每提交一次更新增量 CRC32；<br>3. 后台 `DbDumper` 线程每 10 秒导出快照文本到 `./test_db.o/mem_db.txt`；<br>4. `SnapshotRecover`：通过 TinyRPC 跨节点拉取全量快照覆写。 |
| **`route_impl.h`<br>`route_impl.cc`** | `class RouteImpl` | 静态 3 节点集群路由插件，根据配置列表计算本地 AcceptorID 及 Peer 节点网络地址 ID。 |
| **`tiny_service_impl.h`<br>`tiny_service_impl.cc`** | `class TinyServiceImpl` | 对外业务 RPC 服务实现端，串联 `Certain::Write`、`Certain::Read`、`Certain::Replay` 和 `DbImpl`。 |
| **`log_impl.h`<br>`log_impl.cc`** | `class LogImpl` | 基于本地磁盘文件的按大小滚动日志输出插件。 |
| **`monitor_impl.h`** | `class MonitorImpl` | 默认空实现的监控上报接口。 |
| **`db_type.h`** | 命名空间 `dbtype` | 抹平 RocksDB 与 LevelDB 之间 API 差异的类型别名与适配器。 |

---

### 3.7 基础架构与工具库：`certain/utils/`

高度优化的 C++11 工具原语，是 Certain 能够实现微秒级延迟与零锁争用的幕后英雄。

| 文件名 | 核心类 / 原语 | 核心职责与设计要点 |
| :--- | :--- | :--- |
| **`lock_free_queue.h`** | `class LockFreeQueue<T>` | **环形无锁队列**。基于原子 CAS 推进头尾指针，支持多生产者单消费者（MPSC）并发模式。 |
| **`routine_worker.h`** | `class RoutineWorker<T>` | **多协程事件循环模板基类**。结合 `libco` 和 `epoll`，管理协程池调度空闲任务。 |
| **`traffic_limiter.h`** | `class TrafficLimiter` | **双重令牌桶限流器**。提供 `UseBytes()` 与 `UseCount()`，防止 Catchup 追赶冲垮在线服务。 |
| **`uuid_mng.h`** | `class UuidMng` | **1024 分片 LRU 请求去重器**。缓存达成 Chosen 的 UUID，TTL 为 60 秒，提供幂等写入保证。 |
| **`array_timer.h`** | `class ArrayTimer<T>` | **分级数组时间轮**。提供 $O(1)$ 复杂度的超时插入、移除与触发，负责海量 ClientCmd 的超时检测。 |
| **`co_lock.h`<br>`co_lock.cc`** | `class CoMutex`<br>`class DbEntityLock` | 协程友好的轻量级互斥锁与实体锁，让出 CPU 时挂起协程而非阻塞系统线程。 |
| **`crc32.h`<br>`crc32.cc`** | `crc32()` | 快速循环冗余校验计算，支撑状态机增量 CRC 验证。 |
| **`thread.h`<br>`thread.cc`** | `class ThreadBase`<br>`class ReadWriteLock` | 线程封装基类，提供线程命名、CPU 核心亲和性绑定（Affinity）以及 pthread 读写锁。 |
| **`light_list.h`** | 宏 `LIGHTLIST` | 嵌入式侵入式双向链表，零堆内存分配。 |
| **`mem_pool.h`<br>`mem_pool.cc`** | `class MemPool` | 固定大小内存池，减少系统 `malloc/free` 碎片。 |
| **`singleton.h`** | `class Singleton<T>` | 线程安全的 Meyer's 单例模式基类。 |
| **`usetime_stat.h`** | `class UseTimeStat` | 耗时统计器，自动计算 Avg、P50、P90、P99 分位数。 |
| **`time.h`** | `GetTimeByUsec()` 等 | 获取高精度单调时间戳工具函数。 |

---

### 3.8 样例、压测、工具与脚本层

| 路径分类 | 关键文件 | 核心职责与使用场景 |
| :--- | :--- | :--- |
| **`certain/example/`** | `server.cc` | 3 节点集群守护进程入口。解析 `--index`，启动 PLog、DB、Certain 及 TinyServer。 |
| | `client.cc` | 命令行测试客户端，支持 `--cmd=write/read/appendstring/getstringstatus`。 |
| | `task_perf.cc` | 腾讯原厂高压基准测试程序，支持 10 线程 $\times$ 200 协程超大规模高并发压测。 |
| **`certain/tools/`** | `inspect_plog.cc` | **底层 PLog 解析工具**。直接读取 RocksDB 物理 SST 文件，逐字节 Hex 解码。 |
| | `benchmark_client.cc`| **多线程压测客户端**。支持单/多 Key 切换，精确统计吞吐与 P50/P90/P99 延迟分布。 |
| | `tinyrpc_bench.cc` | **TinyRPC 性能基准程序**。测试纯 RPC 传输时延与极限吞吐。 |
| | `dump_entry.cc` | **在线探测工具**。向节点发起 ToolsService RPC，输出运行态状态机文本。 |
| **`certain/scripts/`** | `manage_cluster.sh` | 集群启停与状态运维管理脚本（`start/stop/restart/clean/status`）。 |
| | `run_deep_dive_experiments.sh` | 一键自动化端到端功能验证实验（单调写、跳洞拒绝、空槽位检查、宕机与自愈）。 |
| | `run_advanced_benchmark.sh` | 梯度并发压测与 `pidstat` CPU 资源采样分析脚本。 |

---

## 4. 全局核心符号速查地图（Symbol Index）

当你需要在代码中寻找某个关键类或方法时，可以对照下表快速定位：

```
[共识与状态机]
Certain::Write / Read / Replay       ──> certain/include/certain/certain.h & certain/src/certain.cc
EntryStateMachine (7大状态管理)       ──> certain/src/entry_state.h & .cc
EntityHelper (共识与命令调度中枢)     ──> certain/src/entity_helper.h & .cc
EntityWorker (单线程无锁驱动事件循环)  ──> certain/src/entity_worker.h & .cc
Pre-Auth 预授权快速路径              ──> certain/src/entity_helper.cc:480 & certain/src/entry_state.cc:257

[数据流向与管线]
AsyncQueueMng (无锁环形队列池)       ──> certain/src/async_queue_mng.h & .cc
LockFreeQueue (原子无锁队列实现)     ──> certain/utils/lock_free_queue.h
Wrapper::SyncWait (libco 协程同步化) ──> certain/src/wrapper.cc:44

[存储与落盘]
EntryKey (24字节大端序对齐 Key)      ──> certain/default/plog_impl.cc:8
PlogImpl::LoadMaxEntry (O(logN)寻址) ──> certain/default/plog_impl.cc:38
PlogWorker::SetRecord (WriteBatch)   ──> certain/src/plog_worker.cc:77
PlogFilter (基于 Compaction 的 GC)   ──> certain/example/task_perf.cc:59
DbWorker::DoJob (状态机提交防线)     ──> certain/src/db_worker.cc:30

[网络与通信]
MsgHeader (8字节二进制包头)          ──> certain/network/msg_header.h:8
WriteItemList (writev 分散写零拷贝)  ──> certain/network/write_item_list.cc:42
PaxosCmd::SwitchToLocalView (半对称) ──> certain/src/command.cc:88
TinyServer (协程 RPC 动态路由)       ──> certain/tiny_rpc/tiny_server.cc:8

[故障自愈与流控]
EntityHelper::TryCatchup (追赶调度)  ──> certain/src/entity_helper.cc:83
CatchupWorker::DoJob (双重令牌桶)    ──> certain/src/catchup_worker.cc:29
DbImpl::SnapshotRecover (快照兜底)   ──> certain/default/db_impl.cc:40
```
