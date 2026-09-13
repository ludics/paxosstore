# 12. Certain 详细源码文件索引地图（全量逐文件解析）

为了使开发者能够按图索骥、精准定位 `certain` 模块中的任意逻辑，本文档对 `certain/` 目录下**每一个源码文件（包括头文件、实现文件、协议定义、系统原语及单元测试）**进行了地毯式的全量索引与职责解析。

---

## 目录速览导航

- [1. 公共接口层 (`certain/include/certain/`)](#1-公共接口层-certainincludecertain)
- [2. 核心共识与流水线层 (`certain/src/`)](#2-核心共识与流水线层-certainsrc)
  - [2.1 状态机、协议命令与调度中枢](#21-状态机协议命令与调度中枢)
  - [2.2 实体与槽位生命周期管理](#22-实体与槽位生命周期管理)
  - [2.3 多线程流水线工作线程 (Workers)](#23-多线程流水线工作线程-workers)
  - [2.4 无锁队列与协程通知管理](#24-无锁队列与协程通知管理)
  - [2.5 核心单元测试与基准性能源码](#25-核心单元测试与基准性能源码)
- [3. 网络通信层 (`certain/network/`)](#3-网络通信层-certainnetwork)
- [4. RPC 框架层 (`certain/tiny_rpc/`)](#4-rpc-框架层-certaintiny_rpc)
- [5. 协议定义层 (`certain/proto/`)](#5-协议定义层-certainproto)
- [6. 默认插件与存储实现层 (`certain/default/`)](#6-默认插件与存储实现层-certaindefault)
- [7. 底层工具与系统原语库 (`certain/utils/`)](#7-底层工具与系统原语库-certainutils)
- [8. 样例服务与原厂压测 (`certain/example/`)](#8-样例服务与原厂压测-certainexample)
- [9. 运维与诊断工具 (`certain/tools/`)](#9-运维与诊断工具-certaintools)
- [10. 自动化脚本与实验套件 (`certain/scripts/`)](#10-自动化脚本与实验套件-certainscripts)

---

## 1. 公共接口层 (`certain/include/certain/`)

该目录定义了 Certain 库对外导出的纯虚接口和配置定义，供上层业务系统（如分布式 KV、表格存储）集成接入。

| 文件路径 | 核心类 / 接口 / 符号 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`certain.h`** | `class Certain` | **引擎顶级静态入口门面（Facade）**。<br>• 提供了库启动停止函数：`Init(...)`、`Start()`、`Stop()`、`Started()`、`Stopped()`；<br>• 提供了客户端读写接口：`Write(...)`（写新值）、`Read(...)`（空槽位安全探针）、`Replay(...)`（提交落后数据并获取最新进度）；<br>• 提供了工具类接口：`Exist(...)`（UUID 去重查询）、`GetWriteValue(...)`（读取特定已选定槽位值）、`EvictEntity(...)`（从内存逐出指定实体）；<br>• 内部实现：将外部静态调用转发给单例 `Wrapper` 统一驱动。 |
| **`options.h`** | `class Options`<br>`class CmdOptions` | **全局参数与单请求配置类**。<br>• `Options`：利用宏 `CERTAIN_OPTION` 定义了包括各种 Worker 线程数（`msg_worker_num`、`entity_worker_num`、`plog_worker_num` 等）、无锁队列容量（`user_queue_size`、`plog_queue_size` 等）、网络与命令超时（`client_cmd_timeout_msec`）、最大重试与追赶批次大小（`max_catchup_num`、`max_replay_num`）、内存容量限制以及 Pre-Auth 预授权开关（`enable_pre_auth`）等配置项；<br>• `CmdOptions`：支持针对单个请求独立定制 `client_cmd_timeout_msec` 超时时间。 |
| **`db.h`** | `class Db` | **业务状态机存储接口（纯虚类）**。<br>• 规范业务数据落盘操作：`Commit(entity_id, entry, value)`，要求业务存储引擎严格顺序应用共识结果；<br>• 状态查询：`GetStatus(entity_id, *max_committed_entry, *flag)` 汇报已提交进度；<br>• 容灾恢复：`SnapshotRecover(...)` 定义当 PLog 已被清理截断时，拉取全量快照恢复状态机的兜底流程；<br>• 细粒度并发控制：提供 `LockEntity` 与 `UnlockEntity` 接口。 |
| **`plog.h`** | `class Plog` | **Paxos 共识日志存储接口（纯虚类）**。<br>• 规范共识决议的持久化存储；<br>• 提供点查与单写：`GetRecord(...)`、`SetRecord(...)`；<br>• 提供高性能批写：`MultiSetRecords(hash_id, records)`，接收多个实体的日志批量落盘；<br>• 提供范围扫描与冷寻址：`RangeGetRecord(...)`、`LoadMaxEntry(...)`（查找最大已持久化 Entry）；<br>• 提供大 Value 负载点查：`GetValue(...)`、`SetValue(...)`。 |
| **`route.h`** | `class Route` | **集群拓扑与地址路由接口（纯虚类）**。<br>• 规范实体在集群节点中的分片与路由定位；<br>• 提供本地监听地址查询：`GetLocalAddr()`；<br>• 提供副本 ID 解析：`GetLocalAcceptorId(entity_id, *acceptor_id)` 计算实体在本地对应的编号（$0 \dots N-1$）；<br>• 提供对端地址查询：`GetServerAddrId(entity_id, acceptor_id, *addr_id)` 获取 Peer 节点的 64 位整数格式网络地址。 |
| **`errors.h`** | `enum RetCodeErrors`<br>`enum NetWorkErrors`<br>`enum ImplErrors`<br>`enum UtilsErrors` | **全系统错误码集中定义枚举**。<br>• 核心状态码：`kRetCodeOk` (0)、`kRetCodeEntryNotMatch` (-3006, 槽位不匹配/跳洞拒绝)、`kRetCodeEntryUncertain` (-3007, 槽位正在落盘未决)、`kRetCodeEntityLoading` (-3017)、`kRetCodeCatchupPending` (-3023, 正在追赶)、`kRetCodeFastFailed` (-3019)、`kRetCodeTimeout` (-3003)；<br>• 网络与底层存储错误：`kNetWorkError` (-2000)、`kImplPlogNotFound` (-4000) 等。 |
| **`log.h`** | `class LogBase` | **日志抽象纯虚基类**。<br>• 规范不同日志级别（`kZero`、`kFatal`、`kError`、`kWarn`、`kInfo`、`kDebug`）的格式化打印接口；<br>• 解耦 Certain 核心代码与具体的终端/文件日志落盘库。 |
| **`monitor.h`** | `class Monitor` | **指标监控与统计上报接口基类**。<br>• 规范核心性能指标监控点上报：包含写耗时（`ReportWriteTimeCost`）、读耗时、追赶流量、无锁队列堆积丢包、UUID 命中率等；<br>• 提供了默认的空实现单例，业务可继承并对接自身 Prometheus/微信 OSS 监控体系。 |

---

## 2. 核心共识与流水线层 (`certain/src/`)

该目录是 Certain 的“大脑”与“中枢神经”，包含了 Multi-Paxos 核心状态机、命令生命周期管理、各职能 Worker 线程及无锁流水线。

### 2.1 状态机、协议命令与调度中枢

| 文件路径 | 核心类 / 结构体 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`entry_state.h`<br>`entry_state.cc`** | `class EntryStateMachine` | **Multi-Paxos 状态机最核心的实现**。<br>• 维护单个槽位的 7 大状态演变（`kNormal`、`kPromiseLocal/Remote`、`kMajorityPromise`、`kAcceptLocal/Remote`、`kChosen`）；<br>• `Promise(bool pre_auth)`：实现严格无冲突提案号公式 $pn = \lfloor\frac{pn + n - 1}{n}\rfloor \cdot n + id + 1$，并为 Pre-Auth 预留 $pn \le n$ 编号区间；<br>• `Accept(...)`：实现 Lamport P2c 不变式，遇到历史已接受提案值时强制锁定继承；<br>• `Update(...)`：接收对端半对称报文合并更新，评估是否达成多数派；<br>• `RestoreValueInRecord(...)`：针对开启 `has_value_id_only` 的轻量元数据记录恢复业务负载。 |
| **`entity_helper.h`<br>`entity_helper.cc`** | `class EntityHelper` | **单实体共识决策与流水线调度中枢**（由 `EntityWorker` 单线程独占持有）。<br>• `HandleClientCmd()`：客户端指令入口，校验 `entry == max_chosen_entry + 1` 拦截乱序写；<br>• `HandleWriteCmd()`：驱动本地 Promise $\to$ 落盘 PLog $\to$ 异步广播；<br>• `HandleReadCmd()`：发起 `check_empty` 探针检查多数派空白；<br>• `HandlePaxosCmd()`：网络半对称报文接入，视角对调后驱动状态机；<br>• `HandleSetFromPlog()` / `HandleGetFromPlog()`：接收 PLog 异步读写回包并推进广播或通知；<br>• `TryCatchup()`：检测日志空洞，按步调度从 Peer 拉取缺失数据。 |
| **`command.h`<br>`command.cc`** | `class CmdBase`<br>`class ClientCmd`<br>`class PaxosCmd` | **跨线程流动的基础命令抽象类族**。<br>• `CmdBase`：队列元素基类，支持 `std::unique_ptr` 零拷贝传递；<br>• `ClientCmd`：业务客户端请求命令，携带 `entity_id`、`entry`、`value`、`uuids`、`timeout_msec` 及协程上下文指针；<br>• `PaxosCmd`：封装 `PaxosMsg` 协议报文，提供 `SwitchToLocalView()` 方法实现**半对称报文在网络到达后的发送方/接收方视角对调**。 |
| **`common.h`** | 常量与通用序列化函数 | • 定义常量：`kInvalidAcceptorId`、`kInvalidEntry`；<br>• `EntryRecordToString()`：将 Protobuf `EntryRecord` 格式化为便于单行 Debug 的文本字符串。 |
| **`msg_serialize.h`<br>`msg_serialize.cc`** | `class MsgSerialize` | **网络报文二进制打包工具**。<br>• 继承自 `SerializeObj`，负责将 `PaxosCmd` 携带的 Protobuf 序列化为网络字节流，并前置拼接固定 8 字节的 `MsgHeader`。 |

### 2.2 实体与槽位生命周期管理

| 文件路径 | 核心类 / 结构体 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`entity_info_mng.h`<br>`entity_info_mng.cc`** | `struct EntityInfo`<br>`class EntityInfoMng`<br>`class EntityInfoGroup` | **实体级别的全局与局部元数据管理器**。<br>• `struct EntityInfo`：记录单个实体的全部运行时游标（`max_chosen_entry` 最大选定槽位、`max_cont_chosen_entry` 最大连续选定槽位、`pre_auth_entry` 预授权租约槽位、`waiting_msg` 等待处理的暂存报文）；<br>• `EntityInfoMng`：`EntityWorker` 线程内部的 `entity_id -> EntityInfo` 映射哈希表；<br>• `EntityInfoGroup`：全局单例，包含 128 个 Shard，提供加锁保护的快速跨线程查询接口（如 `GetMaxChosenEntry`）。 |
| **`entry_info_mng.h`<br>`entry_info_mng.cc`** | `struct EntryInfo`<br>`class EntryInfoMng`<br>`class MemoryLimiter` | **槽位级别的元数据管理器与内存控制**。<br>• `struct EntryInfo`：记录特定 `(entity_id, entry)` 槽位的活动状态（持有 `EntryStateMachine` 状态机实例、`uncertain` 落盘中标记、`broadcast` 待广播标记、`waiting_msgs` 乱序报文暂存桶）；<br>• `EntryInfoMng`：管理未完成共识或刚 Chosen 的活动槽位；<br>• `MemoryLimiter`：限制单个 Worker 中活跃 Entry 的内存上限，并在超限时拒绝新请求，防 OOM。 |

### 2.3 多线程流水线工作线程 (Workers)

| 文件路径 | 核心类 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`entity_worker.h`<br>`entity_worker.cc`** | `class EntityWorker` | **核心实体共识驱动工作线程**（默认 48 个并发）。<br>• 线程入口：`Run()`；<br>• `HandleEvents()`：事件驱动循环，按照优先级依次消费：<br>  1. `user_req_queue_`（客户端请求）<br>  2. `user_rsp_queue_`（结果返回与协程唤醒）<br>  3. `entity_req_queue_`（跨网络收到 Peer 的 Paxos 报文）<br>  4. `plog_rsp_queue_`（PLog 磁盘异步读写完成）<br>  5. `recover_rsp_queue_`（快照恢复完成）；<br>• 根据 `Hash(entity_id)` 绝对绑定单线程，实现单个实体的**全局零锁安全处理**。 |
| **`plog_worker.h`<br>`plog_worker.cc`** | `class PlogWorker`<br>`class PlogReadonlyWorker` | **PLog 异步批量组提交与读取工作线程**。<br>• `PlogWorker`（默认 16 线程）：消费 `plog_req_queue_`，通过 `max_plog_batch_size` 攒批后按 DB 实例 HashId 分组，调用 RocksDB `MultiSetRecords` 执行批量 WriteBatch 刷盘，大幅平摊 fsync 开销；<br>• `PlogReadonlyWorker`（默认 32 线程）：消费 `plog_readonly_req_queue_`，处理实体冷启动加载与范围扫描，确保读写流水线物理隔离。 |
| **`db_worker.h`<br>`db_worker.cc`** | `class DbWorker` | **业务 DB 状态机顺序提交工作线程**（默认 16 线程）。<br>• 消费 `db_req_queue_`；<br>• 严格校验 `cmd->entry() == max_committed_entry + 1`，将已 Chosen 的数据顺序调用业务 `Db::Commit`；<br>• 若检测到空洞（`entry > max_committed_entry + 1`），立即将任务转交 `DbLimitedWorker` 挂起排队。 |
| **`db_limited_worker.h`<br>`db_limited_worker.cc`**| `class DbLimitedWorker` | **业务 DB 乱序缓冲与限流工作线程**。<br>• 消费 `db_limited_req_queue_`，对乱序或积压的 DB 提交请求进行流控排队，防止状态机被瞬时高压击垮。 |
| **`conn_worker.h`<br>`conn_worker.cc`** | `class ConnWorker` | **网络监听专用线程**。<br>• 绑定并在本地 Paxos 互联端口（10066+idx）上循环 `Accept`；<br>• 将建立好的连接送入 `ConnMng` 进行线程池负载分发。 |
| **`conn_mng.h`<br>`conn_mng.cc`** | `class ConnMng` | **连接分配管理器（全局单例）**。<br>• 维护每个 `MsgWorker` 对应的接收队列，将新建的 TCP 连接无锁负载均衡到 48 个 `MsgWorker` 中。 |
| **`msg_worker.h`<br>`msg_worker.cc`** | `class MsgWorker` | **网络 I/O 反应堆工作线程**（默认 48 线程）。<br>• 基于 `Poller`（Linux epoll）监听网络事件；<br>• `HandleRead()` 从套接字读取数据流，通过 `MsgHeader` 校验魔数与长度，反序列化后推入目标 `EntityWorker` 的无锁队列；<br>• `HandleWrite()` 驱动长连接中的发送缓冲区冲刷（Flush）。 |
| **`msg_channel_helper.h`<br>`msg_channel_helper.cc`**| `class MsgChannelHelper` | **网络信道聚合管理工具**。<br>• 维护每个 `MsgWorker` 内部管理的 `MsgChannel` 哈希表，管理 Channel 的激活、移除与事件刷新。 |
| **`catchup_worker.h`<br>`catchup_worker.cc`** | `class CatchupWorker` | **日志追赶工作线程**（默认 8 线程）。<br>• 消费 `catchup_req_queue_`；<br>• 挂载 `TrafficLimiter` 双重令牌桶，对发出的重传数据包实施**字节速率（Bytes/s）**与**请求 QPS（Count/s）**双重限流，休眠避让，保护在线服务不受冲击。 |
| **`recover_worker.h`<br>`recover_worker.cc`** | `class RecoverWorker` | **冷快照恢复工作线程**（默认 7 线程）。<br>• 当节点落后过久、缺失的历史 PLog 已被 GC 物理删除时启动；<br>• 调用 `Db::SnapshotRecover`，通过 TinyRPC 跨节点拉取全量 DB 快照覆写本地，重置实体状态机。 |
| **`tools_worker.h`<br>`tools_worker.cc`** | `class ToolsWorker` | **运维命令处理线程**。<br>• 消费 `tools_req_queue_`，支持在线查询指定实体的内存状态镜像（`DumpEntry`）。 |
| **`wrapper.h`<br>`wrapper.cc`** | `class Wrapper` | **引擎核心总控中枢（全局单例，继承 ThreadBase）**。<br>• 掌管所有 Worker 线程与全局 Manager 的创建、初始化、启动与优雅退出；<br>• 实现 `SyncWait`：利用协程上下文挂起调用线程，并在结果返回时唤醒；<br>• 实现 `Replay`：业务读写前的追齐重放循环，确保 DB 追上连续 Chosen 进度。 |
| **`certain.cc`** | 顶层 Facade 实现 | 将公共接口 `include/certain/certain.h` 中的所有静态方法平铺转发给内部 `Wrapper` 单例。 |

### 2.4 无锁队列与协程通知管理

| 文件路径 | 核心类 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`async_queue_mng.h`<br>`async_queue_mng.cc`** | `class AsyncQueueMng` | **全系统异步无锁队列管线管理中枢（全局单例）**。<br>• 管理包括 `user_req`、`user_rsp`、`entity_req`、`msg_req`、`plog_req`、`plog_rsp`、`db_req` 等 11 类无锁环形队列；<br>• 宏 `CERTAIN_GET_QUEUE` 提供按索引（`ByIdx`）或按实体哈希（`ByEntityId`）快速路由队列实例；<br>• 提供全局队列容量与堆积状态打印（`LogAllQueueStat`）。 |
| **`libco_notify_helper.h`<br>`libco_notify_helper.cc`**| `class LibcoNotifyHelper`<br>`struct LibcoNotifyContext` | **协程调度通知器（基于 Tencent libco）**。<br>• 封装 libco 条件变量 `stCoCond_t`；<br>• `AllocContext()` 分配协程挂起上下文；<br>• `Wait()` 调用 `co_cond_timedwait` 让出当前协程执行权（不阻塞 OS 线程）；<br>• `Notify()` 在共识完成后触发 `co_cond_signal` 唤醒业务协程。 |
| **`notify_helper.h`<br>`notify_helper.cc`** | `class NotifyHelper` | **传统线程通知降级方案**。<br>• 基于 Linux 原生 `eventfd` 或管道 `pipe` 实现跨线程挂起与唤醒，在不使用协程时作为备用通知通道。 |

### 2.5 核心单元测试与基准性能源码

| 文件路径 | 测试目标与验证要点 |
| :--- | :--- |
| **`entry_state_test.cc`** | 针对 `EntryStateMachine` 的全量测试：检验记录合法性校验、提议编号新旧比对、Phase 1/2 状态跃迁及 Pre-Auth 快速路径逻辑。 |
| **`entity_helper_test.cc`** | 模拟完整的单机 Paxos 流水线：测试客户端写入、PLog 回包驱动、网络报文交互、空槽位检查及超时处理。 |
| **`entity_worker_test.cc`** | 测试 `EntityWorker` 事件循环中 5 大无锁队列的消费优先级与内存统计。 |
| **`plog_worker_test.cc`** | 测试 `PlogWorker` 批量组提交、Hash 分组及写错误处理逻辑。 |
| **`db_worker_test.cc`** | 测试 `DbWorker` 状态机顺序提交校验及跳步分流机制。 |
| **`db_limited_worker_test.cc`** | 测试 `DbLimitedWorker` 对乱序 DB 提交的缓冲排队与限流。 |
| **`recover_worker_test.cc`** | 测试 `RecoverWorker` 触发冷备快照恢复的流程与重复保护（`doing_` 表）。 |
| **`msg_worker_test.cc`** | 测试 `MsgWorker` 从网络字节流中解包 `MsgHeader` 并生成 `PaxosCmd`。 |
| **`conn_worker_test.cc`** | 测试 `ConnWorker` 网络监听与连接投递逻辑。 |
| **`conn_mng_test.cc`** | 测试 `ConnMng` 多线程存取 Socket 连接池的并发安全性。 |
| **`msg_channel_helper_test.cc`**| 测试 `MsgChannelHelper` 对多个 TCP 信道事件生命周期的维护。 |
| **`msg_serialize_test.cc`** | 检验 `MsgSerialize` 序列化后的二进制流是否符合 8 字节头部格式。 |
| **`command_test.cc`** | 测试 `PaxosCmd` 的 `SwitchToLocalView` 半对称视角对调是否正确置换记录。 |
| **`common_test.cc`** | 测试通用工具函数与格式化输出。 |
| **`async_queue_mng_test.cc`** | 验证 11 类无锁队列初始化、哈希寻址及高并发存取安全性。 |
| **`libco_notify_helper_test.cc`**| 验证基于 libco 条件变量的协程分配、超时挂起与信号唤醒。 |
| **`notify_helper_test.cc`** | 验证基于 eventfd 的通知池在多线程下的唤醒可靠性。 |
| **`five_replica_test.cc`** | 专门针对 **5 副本集群**的复杂网络交互、多数派选举（3/5）及网络丢包模拟测试。 |
| **`wrapper_test.cc`** | 对 `Wrapper` 主控类的整体生命周期及集成调用进行端到端测试。 |
| **`plog_worker_perf.cc`** | `PlogWorker` 独立刷盘基准测试程序，评估单机纯磁盘 WAL 写入瓶颈。 |
| **`db_worker_perf.cc`** | `DbWorker` 状态机提交独立基准测试，评估内存状态机更新与 CRC 计算的吞吐极限。 |
| **`db_limited_worker_perf.cc`** | 评估在严重空洞积压场景下 `DbLimitedWorker` 的流控吞吐表现。 |

---

## 3. 网络通信层 (`certain/network/`)

该目录实现了轻量级、零拷贝、基于 Linux `epoll` 的对等网络协议栈。

| 文件路径 | 核心类 / 结构体 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`fd_obj.h`** | `class FdObj` | **事件循环抽象基类**。定义了挂载于 epoll 上的文件描述符标准接口：`HandleRead()`（可读回调）与 `HandleWrite()`（可写回调）。 |
| **`inet_addr.h`** | `class InetAddr` | **网络地址封装工具**。<br>• 封装 IPv4 `sockaddr_in`；<br>• 支持将 IP+Port 与 64 位无符号整数（`uint64_t addr_id`）进行无损相互转换，方便在哈希表和协议头中以单个整数传递地址。 |
| **`tcp_socket.h`<br>`tcp_socket.cc`** | `class TcpSocket` | **非阻塞 TCP 套接字封装**。<br>• 封装底层 Linux Socket API；<br>• `InitSocket`：一键设置 `O_NONBLOCK`、`TCP_NODELAY`（禁用 Nagle 算法以降低小包延迟）、调整接收/发送缓冲区；<br>• 提供 `Connect()`、`Listen()`、`Accept()`、`BlockRead()` 等底层网络调用。 |
| **`poller.h`<br>`poller.cc`** | `class Poller` | **Linux epoll 反应堆事件驱动封装**。<br>• 封装 `epoll_create`、`epoll_ctl`、`epoll_wait`；<br>• 提供面向对象的 `AddFd()`、`ModFd()`、`DelFd()`，并将触发事件自动分发给 `FdObj` 的虚函数。 |
| **`msg_header.h`<br>`msg_header.cc`** | `struct MsgHeader` | **8 字节固定网络协议头封装**。<br>• 字段：`magic_num` (1B, 固定 0xfe)、`msg_id` (1B)、`header_len` (2B)、`body_len` (4B，网络大端序)；<br>• 提供序列化、反序列化及网络字节序自动转换方法。 |
| **`msg_channel.h`<br>`msg_channel.cc`** | `class MsgChannel` | **全双工对等网络通信信道**（继承 `FdObj`）。<br>• 每个信道代表一条连接到 Peer 节点的长连接；<br>• `ReadMore()`：读入网络流并解包 8 字节头部，解决粘包与半包问题；<br>• `FlushBuffer()`：驱动底层写链表将缓冲区数据通过网卡发出；<br>• 关联读写缓冲区限制器（`SharedLimiter`）实现背压流控。 |
| **`write_item_list.h`<br>`write_item_list.cc`** | `class WriteItemList`<br>`struct WriteItem` | **高性能零拷贝分散写链表**。<br>• 将离散的待发送数据块以链表形式串联；<br>• `Flush()` 方法将多个分片连续映射为 `struct iovec` 结构数组，一次性调用 Linux 原生 **`writev`** 发出，避免在用户态进行昂贵的大块内存拼包拷贝。 |
| **`poller_test.cc`** 等 | 网络模块单元测试 | 覆盖 `poller`、`tcp_socket`、`msg_channel`、`msg_header`、`write_item_list`、`inet_addr` 的全套网络通信功能测试。 |

---

## 4. RPC 框架层 (`certain/tiny_rpc/`)

基于 Tencent `libco` 协程与 Google Protobuf 打造的高性能全双工 RPC 框架，主要负责业务接入、节点快照传输与运维管理。

| 文件路径 | 核心类 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`tiny_server.h`<br>`tiny_server.cc`** | `class TinyServer` | **协程并发 RPC 服务端**（继承自 `RoutineWorker<TcpSocket>`）。<br>• 绑定本地监听端口，内部默认启动 64 个轻量级协程事件循环；<br>• 每个接入连接被分配到一个协程中以同步代码风格处理；<br>• 通过 Protobuf 反射机制（`CallMethod`）动态路由方法，免除人工 switch-case。 |
| **`tiny_client.h`<br>`tiny_client.cc`** | `class TinyClient`<br>`class TinyChannel`<br>`class TinyController` | **RPC 客户端桩（Stub）**。<br>• `TinyChannel`：继承 `google::protobuf::RpcChannel`，在当前线程/协程中同步阻塞发起网络 RPC 调用，序列化请求并等待服务端响应；<br>• `TinyController`：管理单个 RPC 的返回值（`RetCode`）与状态。 |
| **`tiny_rpc.h`<br>`tiny_rpc.cc`** | `class TinyRpc` | **底层静态 RPC 打包工具类**。<br>• 提供网络协议帧的收发：`ReceiveHeader()`、`ReceiveBody()`、`SendMessage()`、`ReceiveResponse()`。 |
| **`tiny_rpc_test.cc`** | 单元测试 | 测试 TinyRPC 客户端与服务端回环调用的正确性与错误码透传。 |

---

## 5. 协议定义层 (`certain/proto/`)

采用 Google Protocol Buffers（proto2）定义跨网络和持久化数据结构。

| 文件路径 | 核心 Message / Service | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`certain.proto`** | `EntryRecord`<br>`CmdHeader`<br>`PaxosMsg`<br>`RangeCatchupMsg` | **核心 Multi-Paxos 协议规范**。<br>• `EntryRecord`：槽位共识元数据结构，记录 `prepared_num`、`promised_num`、`accepted_num`、`value_id`、`value`、`chosen` 等关键状态，既是网络载荷也是 PLog 物理落盘格式；<br>• `PaxosMsg`：**半对称消息统一结构**，容纳发送方眼中的本地记录 `local_entry_record` 和所知的对端记录 `peer_entry_record`；<br>• `RangeCatchupMsg`：批量日志追赶请求协议。 |
| **`tiny_rpc.proto`** | `service TinyRpcService`<br>`WriteReq/Rsp`<br>`ReadReq/Rsp`<br>`AppendStringReq/Rsp`<br>`GetStringStatusReq/Rsp`<br>`SnapshotRecoverReq/Rsp` | **业务客户端与快照恢复协议规范**。<br>• 暴露业务常用的连续写入、状态查询接口；<br>• 定义节点落后时全量快照传输的 `SnapshotRecover` 接口。 |
| **`tools.proto`** | `service ToolsService`<br>`DumpEntryReq/Rsp` | **运维调试服务规范**。<br>• 允许命令行工具在线指定 `(entity_id, entry)` 探测目标节点在内存中的 Paxos 状态机快照。 |

---

## 6. 默认插件与存储实现层 (`certain/default/`)

包含开箱即用的存储引擎实现、业务 DB 状态机及集群路由插件。

| 文件路径 | 核心类 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`plog_impl.h`<br>`plog_impl.cc`** | `class PlogImpl`<br>`class EntryKey` | **基于 RocksDB 的 PLog 存储引擎官方实现**。<br>• `EntryKey`：紧凑的 24 字节大端序对齐 Key（$8\text{B } entity\_id + 8\text{B } entry + 8\text{B } value\_id$）；<br>• 利用大端序与 RocksDB 字典序一致性，在 `LoadMaxEntry` 中利用 Seek 哨兵向前一步（`iter->Prev()`）实现 **$O(\log N)$ 瞬间定位最大槽位**；<br>• 在 `MultiSetRecords` 中利用 RocksDB 原生 `WriteBatch` 实现高效组提交（Group Commit）。 |
| **`db_impl.h`<br>`db_impl.cc`** | `class DbImpl`<br>`class DbInfo`<br>`class DbDumper` | **1024 桶哈希分片内存业务 DB 实现**。<br>• 每个分片独立读写锁；<br>• 维护 `(entry, crc32, flag)`，每次 Commit 触发**增量 CRC 滚算**，允许副本间直接比对 CRC 判断数据强一致；<br>• 后台 `DbDumper` 线程每 10 秒导出快照到文本文件 `./test_db.o/mem_db.txt`；<br>• 实现 `SnapshotRecover`：通过 TinyRPC 跨节点拉取全量 DB 快照并重置本地状态机。 |
| **`route_impl.h`<br>`route_impl.cc`** | `class RouteImpl` | **静态 3 节点集群路由插件实现**。<br>• 解析传入的 3 个节点 IP:Port 列表，计算当前节点的 AcceptorID（$0, 1, 2$）以及对端节点的 64 位整数网络地址。 |
| **`tiny_service_impl.h`<br>`tiny_service_impl.cc`** | `class TinyServiceImpl` | **客户端业务服务服务端落地实现**。<br>• 继承自 `TinyRpcService`；<br>• 将客户端的 `Write`、`Read`、`AppendString`、`GetStringStatus` 请求串联至 `Certain::Write/Read/Replay` 与 `DbImpl`。 |
| **`log_impl.h`<br>`log_impl.cc`** | `class LogImpl` | 基于本地磁盘文件的线程安全日志实现，支持按文件大小自动滚动。 |
| **`monitor_impl.h`** | `class MonitorImpl` | 监控上报实现，将耗时与计数器打点汇总。 |
| **`db_type.h`** | 命名空间 `dbtype` | 统一 RocksDB 与 LevelDB 的类型别名（如 `Slice`、`DB`、`Iterator`、`Status`），方便不同存储后端的适配切换。 |
| **`*_test.cc`** | 插件单元测试 | 包含 `plog_impl_test`、`db_impl_test`、`route_impl_test`、`log_impl_test` 等插件的独立正确性测试。 |

---

## 7. 底层工具与系统原语库 (`certain/utils/`)

为了在百万级 QPS 和微秒级延迟下避免锁竞争和内核态开销，Certain 精心打造了系列高性能系统原语。

| 文件路径 | 核心类 / 原语 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`lock_free_queue.h`** | `class LockFreeQueue<T>` | **高性能环形无锁队列（核心骨干）**。<br>• 基于原子 CAS 推进头尾索引，支持多生产者单消费者（MPSC）高并发模式，是流水线间消息传递的零锁底座。 |
| **`routine_worker.h`** | `class RoutineWorker<T>` | **多协程事件循环模板基类**。<br>• 继承自 `ThreadBase`，深度结合 `libco` 与 `epoll`，将任务自动分发给空闲协程执行，实现高并发任务处理。 |
| **`array_timer.h`** | `class ArrayTimer<T>` | **分级数组时间轮定时器**。<br>• 利用毫秒级数组环提供 **$O(1)$ 复杂度的超时插入、移除与到期检测**，驱动海量 ClientCmd 的超时淘汰。 |
| **`traffic_limiter.h`** | `class TrafficLimiter`<br>`class CountLimiter` | **双重令牌桶流控限流器**。<br>• 提供平滑的 `UseBytes(n)`（带宽限流）和 `UseCount()`（QPS 限流），负责保护故障自愈追赶流量不冲垮在线系统。 |
| **`capacity_limiter.h`** | `class CapacityLimiter`<br>`class SharedLimiter` | **内存容量安全配额器**。<br>• 提供层级化的内存申请与释放计数，防止网络接收缓冲区与待发送链表发生无节制的内存膨胀。 |
| **`lru_table.h`** | `class LruTable<K, V>` | **基于哈希与双向链表的经典 LRU 缓存表**，支持最大元素上限与访问更新淘汰。 |
| **`uuid_mng.h`** | `class UuidMng` | **1024 分片全局并发请求去重管理器**。<br>• 内部由 1024 个分片的带锁 LRU 组成；<br>• 将达成 Chosen 的请求 UUID 缓存 60 秒，客户端重试时调用 `Exist` 即可快速判定，实现**全局写入幂等性**。 |
| **`co_lock.h`<br>`co_lock.cc`** | `class CoMutex`<br>`class DbEntityLock` | **协程专用互斥锁与 RAII 实体锁**。<br>• 竞争失败时挂起当前 libco 协程让出 CPU，而非阻塞 OS 线程；<br>• `DbEntityLock` 提供实体级别的读写锁保护。 |
| **`crc32.h`<br>`crc32.cc`** | `crc32(...)` | 高性能硬件/软件循环冗余校验计算，支撑业务 DB 的增量 CRC32 状态比对。 |
| **`thread.h`<br>`thread.cc`** | `class ThreadBase`<br>`class ReadWriteLock`<br>`class Mutex` | **C++11 线程基类与 pthread 锁封装**。<br>• 提供线程命名、CPU 核心亲和性绑定（`SetAffinity`）、优雅停机等待（`WaitExit`）。 |
| **`mem_pool.h`<br>`mem_pool.cc`** | `class MemPool` | **固定大小内存池**。<br>• 预分配大块连续内存，通过自由索引列表进行快速借还，避免频繁系统 `malloc/free` 产生内存碎片。 |
| **`light_list.h`** | 宏 `LIGHTLIST` | **轻量级侵入式双向链表**。<br>• 采用宏定义实现的链表结构，零额外堆内存分配，直接嵌入到宿主结构体中。 |
| **`hash.h`** | `Hash(...)` | 64 位无符号整数快速哈希散列函数，为实体分片寻址提供均匀分布。 |
| **`time.h`** | `GetTimeByUsec()` 等 | 获取系统高精度单调时钟时间戳（微秒/毫秒/秒），提供计时器类 `TimeDelta`。 |
| **`usetime_stat.h`** | `class UseTimeStat` | 耗时分位数统计器，自动统计并输出请求耗时的 Min、Avg、Max 及 P50/P90/P99。 |
| **`singleton.h`** | `class Singleton<T>` | 线程安全的 Meyer's 单例模式模板基类。 |
| **`memory.h`** | `unique_cast(...)` | 智能指针类型安全向下转换工具函数。 |
| **`macro_helper.h`** | 常用宏定义 | 包含属性 Getter/Setter 自动生成宏及禁止拷贝赋值宏。 |
| **`header.h`** | 标准库常用头文件聚合包含。 |
| **`*_test.cc`** | 工具库单元测试 | 对上述所有底层系统原语均配备了高并发与极限边缘场景的单元测试。 |

---

## 8. 样例服务与原厂压测 (`certain/example/`)

展示了完整的服务启动集成代码以及腾讯官方的并发基准测试模型。

| 文件路径 | 核心程序 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`server.cc`** | 3 节点集群守护进程 | **独立单节点服务端主程序**。<br>• 解析 `--index=0/1/2` 与 `--data_dir`；<br>• 初始化 RocksDB PLog、DbImpl、Certain 引擎；<br>• 启动业务 `TinyServer`（端口 11066+idx）和运维 `ToolsServer`（端口 12066+idx）；<br>• 注册 SIGTERM/SIGINT 优雅停机信号处理。 |
| **`client.cc`** | 命令行测试客户端 | **交互式客户端测试工具**。<br>• 支持 `--cmd=write`：指定 `(entity_id, entry, value)` 发起写入；<br>• 支持 `--cmd=read`：发起空槽位探针读取；<br>• 支持 `--cmd=appendstring`：自动 Replay 追齐后追加写入；<br>• 支持 `--cmd=getstringstatus`：Replay 追齐后读取最新业务数据与 CRC 校验码。 |
| **`task.h`<br>`task.cc`** | `class Task` | **压测任务执行体**。<br>• 封装压测线程任务，支持多协程并行执行 `WriteTask`、`ReadTask` 与并发冲突测试 `ConflictTask`。 |
| **`task_perf.cc`** | 原厂高并发压测程序 | **腾讯原厂基准压测入口**。<br>• 启动 10 个 Task 线程，每个线程开启 **200 个 libco 协程**（全集群 2,000 个在途并发请求）；<br>• 每个协程独占一个独立的 `entity_id` 进行海量并发压测；<br>• 内置 `PlogFilter` 演示了基于 RocksDB Compaction 的历史 PLog 自动垃圾回收机制。 |

---

## 9. 运维与诊断工具 (`certain/tools/`)

开发与运维过程中用于状态穿透排查与性能评测的实用工具。

| 文件路径 | 核心工具程序 | 提供了什么 & 实现了什么 |
| :--- | :--- | :--- |
| **`inspect_plog.cc`** | PLog 物理文件解析器 | **底层存储诊断工具**。<br>• 直接以只读模式打开 RocksDB 数据目录（如 `test_plog.o`）；<br>• 遍历底层 SST 文件，按 24 字节大端序对齐解码 Key，输出 16 进制 Raw Hex；<br>• 反序列化 Protobuf `EntryRecord`，直观打印每个槽位的 `prepared/promised/accepted/chosen` 物理落盘记录。 |
| **`benchmark_client.cc`**| 多线程阶梯压测工具 | **端到端基准压测客户端**。<br>• 支持多线程并发发压；<br>• 支持单 Key（`--single_entity=true`）与多 Key（多 Entity）模式自由切换；<br>• 输出高精度的耗时直方图、QPS 吞吐以及 P50 / P90 / P99 延迟分位数。 |
| **`tinyrpc_bench.cc`** | TinyRPC 纯性能基准测试 | **RPC 框架性能评测工具**。<br>• 排除磁盘 I/O 与 Paxos 逻辑干扰，单机回环压测 TinyRPC 纯框架协议编解码与协程调度开销（实测单机超 30,000 QPS，平均网络延迟 130μs）。 |
| **`dump_entry.cc`** | 在线内存状态机快照工具 | **线上运维探测工具**。<br>• 通过 TinyRPC 连接正在运行的 Server 运维端口（12066+idx），向 `ToolsService` 发起查询并打印指定槽位在内存中的 Paxos 运行态快照。 |
| **`tools_service.h`<br>`tools_service.cc`**| 运维 RPC 服务端实现 | 挂载于 `TinyServer` 上的运维服务实现端，将 `DumpEntry` 请求映射到底层 `Certain::DumpEntry`。 |

---

## 10. 自动化脚本与实验套件 (`certain/scripts/`)

| 脚本文件名 | 核心用途与主要执行逻辑 |
| :--- | :--- |
| **`manage_cluster.sh`** | **集群启停管理脚本**。<br>• 支持 `start`、`stop`、`restart`、`clean`、`status` 操作，管理本地 3 节点进程的启停与 PID 文件。 |
| **`run_3node_demo.sh`** | **官方 3 节点 Demo 运行脚本**。<br>• 快速启动 3 副本集群并演示单次写入、状态读取与结果校验。 |
| **`run_deep_dive_experiments.sh`** | **全流程深度实战验证套件**。<br>• 自动拉起集群并顺序执行 10 个测试阶段：干净启动 $\to$ 连续写入 $\to$ 乱序跳洞拦截 $\to$ 空槽位检查 $\to$ Replay 追加 $\to$ 内存状态 dump $\to$ 杀死单节点 $\to$ 多数派存活写入 $\to$ 节点重启自愈跟踪 $\to$ 归档磁盘落盘产物。 |
| **`run_benchmark.sh`** | **多线程基准压测脚本**。<br>• 针对运行中的集群自动执行 4 线程与 8 线程基准写入压测并输出报告。 |
| **`run_advanced_benchmark.sh`** | **高级梯度压测与 CPU 采样脚本**。<br>• 梯度测试 1、4、8、16、32、64 线程下的吞吐与延迟；<br>• 自动使用 `pidstat -u 1` 采样 Node 0/1/2 服务端进程的内核态（`%system`）与用户态（`%usr`）CPU 资源开销。 |
