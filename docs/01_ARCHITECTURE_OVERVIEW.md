# 01. Certain 架构全景与多线程流水线设计

## 1. 为什么是 Entity-based "PaxosLog + DB"？

### 1.1 微信业务特征与全局单日志流（Single-Log）的困境
在典型的强一致性共识系统（如 Raft 实现 etcd、ZooKeeper 的 Zab、单 Paxos 组的 Chubby）中，集群通常维护**单条全局有序的日志流**。所有写入操作都必须排队追加到全局 Log 的尾部，由单一 Leader 进行序列化。

然而，在微信的核心业务场景下（见 VLDB 2017 论文 *PaxosStore: High-availability Storage Made Practical in WeChat* Section 1 & 2）：
- 每日活跃用户超过 7 亿（日请求量数十亿至千亿级别）；
- 单个用户的朋友圈、群聊、用户资料、支付订单天然具有高并发、互不相关的特征；
- 99% 以上的访问都是**单记录（Single-Record）点查或追加**。

如果使用单一全局日志流，整个集群的吞吐量将被**全局锁争用**和**磁盘顺序写瓶颈**死死卡住。

### 1.2 Entity-based 独立 Paxos Log
为了突破扩展性瓶颈，PaxosStore 提出了 **Entity-based Multi-Paxos** 范式：
- **Entity**：以实体为单位（例如一个微信号、一个群 ID、一个订单号），赋予一个 64 位无符号整数 `entity_id`；
- **独立日志流**：每一个 `entity_id` 拥有自己完全独立的 Paxos Log 序列号（从 `entry = 1` 开始递增）；
- **水平并行**：不同 `entity_id` 之间的 Paxos 提案、表决、持久化完全并发，互不影响；
- **分片与扩展**：数以亿计的 `entity_id` 均匀哈希分布到多机和不同的工作线程中，彻底消除全局串行化锁瓶颈。

```
                    ┌───────────────────────────────┐
                    │     Client Request Stream     │
                    └───────────────┬───────────────┘
                                    │
                  ┌─────────────────┴─────────────────┐
                  ▼                                   ▼
        Entity 10001 (User A)               Entity 10002 (User B)
     ┌───────────────────────┐           ┌───────────────────────┐
     │ Entry 1: Paxos Chosen │           │ Entry 1: Paxos Chosen │
     │ Entry 2: Paxos Chosen │           │ Entry 2: In-Progress  │
     │ Entry 3: In-Progress  │           │ ...                   │
     └───────────────────────┘           └───────────────────────┘
                 │                                   │
                 ▼                                   ▼
         Apply to DB Shard                   Apply to DB Shard
```

### 1.3 PaxosLog 与 DB（状态机）的解耦
PaxosStore 将存储体系严格划分为两层：
1. **Plog（Paxos Log）**：
   - 记录共识过程的中间态与决策（Promise、Accept、Chosen）；
   - 作为节点故障重启、网络丢包时数据恢复与日志追赶的唯一真理来源（Source of Truth）；
   - 底层由高性能 KV 引擎（默认使用 RocksDB）支撑。
2. **DB（业务状态机）**：
   - 承载业务读取与最终数据状态（如用户关系、消息列表、余额）；
   - **严格单调应用（Linearizable Apply）**：只有在 `entry == max_cont_chosen_entry + 1`（无日志空洞）时，共识层才将数据提交至 DB；
   - 读请求通常直接读取最新 Apply 的 DB 状态，实现超低延迟（< 20ms）。

---

## 2. 核心架构与多线程流水线（Pipeline）

Certain 采用了典型的 **Pipeline（流水线）+ SEDA（分阶段事件驱动）** 架构。所有跨阶段数据流转完全由**无锁环形队列**驱动，杜绝 OS 级互斥锁引发的上下文切换。

### 2.1 整体线程与队列数据流向图

```
                   [ Client 调用 Certain::Write ]
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │      libco 协程       │
                     │  (包装为 ClientCmd)   │
                     └───────────┬───────────┘
                                 │ PushByMultiThread
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │                     AsyncQueueMng                         │
   │  ┌───────────────────────┐     ┌───────────────────────┐  │
   │  │   user_req_queues_    │     │   entity_req_queues_  │  │
   │  └───────────┬───────────┘     └───────────▲───────────┘  │
   └──────────────┼─────────────────────────────┼──────────────┘
                  │ Hash(entity_id)             │
                  ▼                             │ 网络接收
   ┌────────────────────────────────────────────┴──────────────┐
   │             EntityWorker (核心业务共识中枢)               │
   │   - 维护 EntityInfo / EntryStateMachine 状态机            │
   │   - 单线程免锁处理特定 entity_id 集合                    │
   └──────────────┬──────────────┬──────────────┬──────────────┘
                  │              │              │
      PlogReq     │      MsgReq  │       DbReq  │
                  ▼              ▼              ▼
   ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
   │   PlogWorker    │  │    MsgWorker    │  │    DbWorker     │
   │ (RocksDB 刷盘)  │  │ (网络 epoll 收发)│  │ (应用到业务 DB) │
   └─────────────────┘  └─────────────────┘  └─────────────────┘
```

### 2.2 十大专用 Worker 线程职责表

在 `certain/src/wrapper.cc` 中的 `Wrapper::InitWorkers` 函数中，初始化了系统运行所需的所有线程：

```265:308:certain/src/wrapper.cc
int Wrapper::InitWorkers() {
  workers_.clear();

  workers_.push_back(std::make_unique<ConnWorker>(options_));

  for (uint32_t i = 0; i < options_->msg_worker_num(); ++i) {
    workers_.push_back(std::make_unique<MsgWorker>(options_, i));
  }

  for (uint32_t i = 0; i < options_->entity_worker_num(); ++i) {
    workers_.push_back(std::make_unique<EntityWorker>(options_, i));
  }

  for (uint32_t i = 0; i < options_->plog_worker_num(); ++i) {
    workers_.push_back(
        std::make_unique<PlogWorker>(options_, i, plog_impl_, db_impl_));
  }

  for (uint32_t i = 0; i < options_->plog_readonly_worker_num(); ++i) {
    workers_.push_back(std::make_unique<PlogReadonlyWorker>(
        options_, i, plog_impl_, db_impl_));
  }

  for (uint32_t i = 0; i < options_->db_worker_num(); ++i) {
    workers_.push_back(std::make_unique<DbWorker>(options_, i, db_impl_));
  }

  for (uint32_t i = 0; i < options_->db_limited_worker_num(); ++i) {
    workers_.push_back(
        std::make_unique<DbLimitedWorker>(options_, i, db_impl_));
  }

  for (uint32_t i = 0; i < options_->recover_worker_num(); ++i) {
    workers_.push_back(std::make_unique<RecoverWorker>(options_, i));
  }

  for (uint32_t i = 0; i < options_->catchup_worker_num(); ++i) {
    workers_.push_back(std::make_unique<CatchupWorker>(options_, i));
  }

  for (uint32_t i = 0; i < options_->tools_worker_num(); ++i) {
    workers_.push_back(std::make_unique<ToolsWorker>(options_, i));
  }

  return 0;
}
```

| Worker 类名 | 默认并发数 | 核心职责 |
| :--- | :--- | :--- |
| **`ConnWorker`** | 1 | 监听本地 Paxos 互联端口（默认 10066+idx），接受集群副本连接并分发给 MsgWorker。 |
| **`MsgWorker`** | 48 | 基于 Linux `epoll` 的高性能 I/O 线程，维护副本间长连接，序列化/反序列化 `PaxosMsg`。 |
| **`EntityWorker`** | 48 | **系统核心脑裂规避与共识决策中枢**。持有 `EntityHelper`，执行 Multi-Paxos 状态机迁移。 |
| **`PlogWorker`** | 16 | 异步写入 Plog 日志，聚合同一 Hash 分组的写操作，执行 RocksDB WriteBatch 刷盘。 |
| **`PlogReadonlyWorker`**| 32 | 专用于处理只读与加载请求（如初次访问某个 Entity 时加载元数据，避免阻塞写流水线）。 |
| **`DbWorker`** | 16 | 将连续 Chosen 的 `entry` 线性化地 Commit 到业务 DB 状态机。 |
| **`DbLimitedWorker`** | 16 | 遇到日志空洞时，对跳步的 DB 请求进行缓冲排队与流控。 |
| **`CatchupWorker`** | 8 | 节点故障或网络落后时，受令牌桶限流保护地从多数派 Peer 拉取缺失日志。 |
| **`RecoverWorker`** | 7 | 副本损坏时进行冷快照传输恢复。 |
| **`ToolsWorker`** | 1 | 运维管理工具接口（如响应 `dump_entry` 查看内存状态机）。 |

---

## 3. 实体级无锁隔离（Zero-Lock Partitioning）

在多核服务器上，最影响高并发性能的因素就是锁争用。Certain 采用了**确定性实体哈希分区**：
在 `certain/src/async_queue_mng.h` 中：

```58:69:certain/src/async_queue_mng.h
#define CERTAIN_GET_QUEUE(fname, name, queue_cnt)     \
  AsyncQueue* fname##ByIdx(uint32_t id) {             \
    assert(id < queue_cnt);                           \
    assert(name[id] != nullptr);                      \
    return name[id].get();                            \
  }                                                   \
  AsyncQueue* fname##ByEntityId(uint64_t entity_id) { \
    uint32_t id = Hash(entity_id) % queue_cnt;        \
    assert(name[id] != nullptr);                      \
    return name[id].get();                            \
  }
```

每个队列都是 `LockFreeQueue<CmdBase>`（无锁环形队列）。
对于任意一个实体 `entity_id`：
$$\text{WorkerIndex} = \text{Hash}(entity\_id) \pmod{\text{entity\_worker\_num}}$$
- 该实体发来的客户端请求（`user_req`）进入 `user_req_queues_[WorkerIndex]`；
- 来自对端副本的 Paxos 投票网络报文（`entity_req`）进入 `entity_req_queues_[WorkerIndex]`；
- 本地 Plog 刷盘完成的回调（`plog_rsp`）进入 `plog_rsp_queues_[WorkerIndex]`。

**数学保证**：所有涉及同一个 `entity_id` 的状态变更，永远且仅被固定的同一个 `EntityWorker` 线程串行调度。因此，在 `EntityHelper` 内部操作 `EntityInfo`、`EntryStateMachine` 时，**完全不需要任何互斥锁（Mutex）**，性能达到极致！

---

## 4. 同步 API 与异步引擎的胶水：libco 协程

Certain 对外暴露的 API（如 `Certain::Write`、`Certain::Read`、`Certain::Replay`）从业务方角度看是**阻塞式**的函数调用，方便业务编写线性的业务逻辑。但在引擎内部，它是完全**异步非阻塞**的。
这一桥梁是通过腾讯开源的轻量级协程库 **`libco`** 实现的。

### 4.1 SyncWait 调用流转
在 `certain/src/wrapper.cc` 中：

```44:59:certain/src/wrapper.cc
int SyncWait(ClientCmd* cmd) {
  auto ctx = libco_notify_helper->AllocContext();
  cmd->set_context(ctx.get());

  std::unique_ptr<ClientCmd> dummy(cmd);
  int ret = PushClientCmd(dummy);
  if (ret != 0) {
    dummy.release();
    return ret;
  }
  libco_notify_helper->Wait(ctx);
  return cmd->result();
}
```

1. **AllocContext**：从 `LibcoNotifyHelper` 中分配一个协程条件变量上下文 `LibcoNotifyContext`，记录在 `ClientCmd` 中；
2. **PushClientCmd**：将 `ClientCmd` 压入对应的 `user_req_queues_`；
3. **libco_notify_helper->Wait(ctx)**：调用 `co_cond_timedwait` 让出当前协程的执行权（CPU 立即被调度去处理其他网络连接或协程，操作系统线程不休眠）；
4. **EntityWorker 达成共识**：当 Paxos 投票达到多数派并持久化后，`EntityWorker` 在 `ReplyClientCmd` 中执行：
```cpp
void EntityWorker::ReplyClientCmd(ClientCmd* client_cmd) {
  auto ctx = client_cmd->context();
  LibcoNotifyHelper::GetInstance()->Notify(ctx);
}
```
5. **Notify & 唤醒**：`co_cond_signal` 触发，业务协程被重新调度，`SyncWait` 从 `Wait` 处返回，直接得到 `cmd->result()`，整个调用链条毫秒级闭环！
