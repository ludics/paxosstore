# 09. 深入故障自愈：日志空洞、Catchup 追赶与 Snapshot Recover

在分布式系统中，“故障是常态，正常是特例”。网络分区、机器断电、硬件坏道、进程崩溃随时可能发生。

本篇文档将重点针对以下核心疑问进行深度剖析：
1. **Paxos 理论允许日志空洞，为什么 Certain 客户端跳步写入会返回 `-3006`（`kRetCodeEntryNotMatch`）？**
2. **Certain 中空洞到底是如何产生的？系统如何保证状态机的线性一致性？**
3. **节点落后时，Catchup 追赶是如何通过流控机制自愈的？**
4. **当旧 PLog 已经被 GC 物理删除导致 Catchup 报 `NotFound` 时，Recover 机制如何拉取全量快照完成兜底恢复？**

---

## 1. 深度辨析：理论 Paxos 日志空洞 vs 存储系统跳步写

### 1.1 经典 Paxos 论文中的“日志空洞”（Log Hole）
在 Leslie Lamport 的经典论文 *《Paxos Made Simple》*（Section 3: Implementing a State Machine）中，分布式状态机的复制原理如下：
- 系统由一系列 Paxos 实例（Instance / Entry）组成：Instance 1, Instance 2, Instance 3...
- 理论上，不同 Instance 之间的决议可以**并发乱序发起**。例如，某个 Proposer 可以同时就 Instance 1、2、3 发起提议；
- **空洞的产生**：可能 Instance 1 和 Instance 3 已经达成 Chosen，但 Instance 2 由于网络丢包、冲突推迟，尚未选出值。
- **理论补齐方案**：当状态机执行到 Instance 2 时，由于缺少该指令而无法执行 Instance 3。此时 Leader 可以主动就 Instance 2 发起一轮提案，提议一个**空操作（no-op / null value）**，将其迅速推至 Chosen。一旦 Instance 2 达成 no-op，状态机即可跳过该槽位，继续执行 Instance 3。

---

### 1.2 为什么 Certain 客户端跳步写会返回 `-3006`（`kRetCodeEntryNotMatch`）？

在我们的实验中：向 Entity 30001 写入 Entry 1 后，客户端故意尝试直接写入 Entry 3，结果直接被拒：
```
Write E(30001, 3) value.sz 11 ret -3006
```

#### 根本原因剖析：
1. **对外 API 语义必须严格单调递增**：
   在存储系统（如分布式 KV、消息队列）中，客户端发起的不是“任选槽位提议”，而是**业务逻辑的连续追加（Append / Sequential Write）**。
   如果客户端请求写 Entry 3，但本地只决议到 Entry 1，此时 Certain 的判断逻辑是：
   ```cpp
   // certain/src/entity_helper.cc 第 260-267 行
   uint64_t matched_entry = entity_info->max_chosen_entry + 1;
   if (entry != matched_entry) {
     FinishClientCmd(entity_info, kRetCodeEntryNotMatch);
     TryCatchup(entity_info, __LINE__);
     return kRetCodeEntryNotMatch;
   }
   ```
2. **防御并发脑裂与客户端乱序**：
   客户端发来跳步的 Entry 3，只可能有两种情况：
   - **情况 A（本地落后了）**：其他存活副本（多数派）早已默默把 Entry 2 决议完了，只是当前连接的这台机器还没收到同步报文；
   - **情况 B（客户端发错了）**：存在并发客户端写，或者客户端自己的序号算错了。
   Certain 绝不会允许直接在本地挖一个 Entry 2 的空洞去迁就客户端！它返回 `-3006`（`kRetCodeEntryNotMatch`），其语义是：**“本地最新已确认的槽位是 `max_chosen_entry`，你提交的序号与预期不符！”**
3. **顺手触发后台自愈（`TryCatchup`）**：
   在返回 `-3006` 的同时，代码立即调用了 `TryCatchup(entity_info)`！本节点会立刻向其他节点拉取缺失的 Entry 2。如果是情况 A，数毫秒内 Entry 2 被拉回并 Chosen，此时客户端再次重试写入 Entry 3 即可瞬间成功！

---

### 1.3 Certain 到底允不允许空洞存在？

答案是：**“PLog 存储与网络传输层完全允许空洞存在，但业务 DB 状态机层绝不允许跨越空洞 Apply！”**

#### 双游标机制（Dual-Cursor）：
在 `EntityInfo` 中维护了两个关键游标：
- **`max_chosen_entry`**：本节点已知的**最大已选定槽位**（可能存在空洞）；
- **`max_cont_chosen_entry`**：本节点已知的**最大连续已选定槽位**（无空洞前缀）。

#### 状态机安全铁律：
假设节点因为网络闪断，错过了 Entry 2，但接收到了 Entry 3 的 Chosen 报文：
- 此时 `max_cont_chosen_entry = 1`，而 `max_chosen_entry = 3`（存在一个 Entry 2 的空洞）；
- **PLog 层**：允许将 Entry 3 持久化到 RocksDB；
- **DB 状态机层**：在 `certain/src/entity_helper.cc` 中：
  ```cpp
  if (entity_info->max_cont_chosen_entry + 1 == info->entry) {
    entity_info->max_cont_chosen_entry++;
    DbWorker::GoToDbReqQueue(client_cmd); // 只有连续自增时才允许 Commit DB
  }
  ```
  因为 `1 + 1 != 3`，Entry 3 绝对不会被送往 `DbWorker`！应用层状态机被坚决锁死在 Entry 1，杜绝了乱序执行导致的业务数据灾难！

---

## 2. Catchup 追赶自愈机制深度剖析

当系统检测到 `max_cont_chosen_entry < max_chosen_entry` 时，表明存在日志空洞，系统启动 Catchup 追赶流程。

```
                       [ 判定日志落后 ]
                max_cont_chosen_entry < max_chosen_entry
                                │
                                ▼
                       [ EntityHelper::TryCatchup ]
                                │
               ┌────────────────┴────────────────┐
               ▼                                 ▼
      [ 本地 PLog 补齐 ]                 [ 跨节点 Peer 补齐 ]
  (已落盘但 DB 尚未 Commit)              (本地缺失该 Entry 记录)
               │                                 │
               ▼                                 ▼
         投递 DbWorker                   投递 CatchupWorker
       (串行线性化 Commit)               (受 TrafficLimiter 限流)
                                                 │
                                                 ▼
                                        广播 catchup=true 报文
                                                 │
                                                 ▼
                                        Peer 节点回发 Entry 记录
                                                 │
                                                 ▼
                                        持久化 PLog 并推进连续游标
```

### 2.1 早期实现与当前开源版本的演进对比

在早期博客中提到的概念与当前最新源码的对应关系如下：

| 早期版本概念 (如博客所述) | 当前最新 C++11 实现 | 核心职能与机制 |
| :--- | :--- | :--- |
| **`EntityCatchUp`** | `Wrapper::Replay` / `IterateAndCatchup` | 将状态机已 Chosen 但尚未 Apply 的连续 Entry 顺序 Commit 到 DB。 |
| **`CheckForCatchUp`** | `EntityHelper::TryCatchup` | 检查并推进 `max_cont_chosen_entry` 到 `max_chosen_entry`，跨节点拉取缺失日志。 |
| **`RangeLoadFromPLog`**| `PlogImpl::RangeGetRecord` | 在本地或 Peer 节点以批量连续范围扫描拉取缺失的 Entry 记录。 |

---

### 2.2 跨节点日志追赶：`CatchupWorker` 与双重令牌桶限流

在实际生产中，如果一台挂机半天的节点重启，瞬间向集群请求成千上万条缺失的日志，巨大的网络包和磁盘读取很容易冲垮正在承载业务请求的健康节点。

Certain 在 `certain/src/catchup_worker.cc` 中设计了**极其严格的双重限流器（`TrafficLimiter`）**：

```29:48:certain/src/catchup_worker.cc
void CatchupWorker::DoJob(std::unique_ptr<PaxosCmd> job) {
  // traffic limiter
  uint64_t sleep_ms = 0;
  // 1. 流量字节速率限流 (Token Bucket: Bytes/s)
  do {
    sleep_ms = limiter_.UseBytes(job->SerializedByteSize());
    if (sleep_ms > 0) {
      monitor_->ReportCatchupTotalFlowLimit();
    }
    poll(nullptr, 0, sleep_ms);
  } while (sleep_ms);

  // 2. 请求 QPS 频次限流 (Token Bucket: Count/s)
  do {
    sleep_ms = limiter_.UseCount();
    if (sleep_ms > 0) {
      monitor_->ReportCatchupTotalCountLimit();
    }
    poll(nullptr, 0, sleep_ms);
  } while (sleep_ms);

  MsgWorker::GoAndDeleteIfFailed(std::move(job));
}
```

#### 限流保护设计：
1. **字节流控（`UseBytes`）**：限制追赶报文占用的最大网络带宽，避免打爆跨机房专线；
2. **频率流控（`UseCount`）**：限制每秒发出的最大 Catchup 报文数，避免接收端 `MsgWorker` 队列被刷爆；
3. 一旦超出配额，自动调用 `poll(nullptr, 0, sleep_ms)` 主动休眠让出 CPU，确保**正常在线业务流量的绝对优先级**！

---

## 3. Snapshot Recover 全量快照恢复机制

### 3.1 为什么必须有 Recover？（Catchup 的失效困境）
在上一篇文档中，我们介绍了 PLog 的垃圾回收（GC）：
- 为了防止磁盘打满，已经提交到业务 DB 的历史 PLog 会被定期清理；
- **极端故障场景**：
  假设节点 A 宕机了整整 7 天，在此期间节点 B 和 C 已经决议并提交了 100 万条日志，且这些历史 PLog 已经全部被 RocksDB Compaction 物理删除了；
  节点 A 重启，向 B 发起 Catchup，请求获取从 Entry 100 到 Entry 1000100 的日志；
  节点 B 翻查自己的 RocksDB PLog，只能返回 **`NotFound` (`kImplPlogNotFound`)**！
- 此时增量日志追赶彻底无能为力，节点 A 陷入死局。

---

### 3.2 快照恢复全流程（`RecoverWorker` 与 `SnapshotRecover`）

为了在增量日志丢失时依然能够自愈，Certain 提供了**兜底的物理级逃生通道 —— `RecoverWorker`**（位于 `certain/src/recover_worker.cc`）。

```
[ 落后节点 A ]                                              [ 健康节点 B ]
      │                                                           │
      │── 1. Catchup 请求缺失 PLog ──────────────────────────────>│
      │<── 2. 返回 NotFound (历史 PLog 已被 GC 删除) ─────────────│
      │                                                           │
      │── 3. 判定增量失效, 触发 TriggerRecover                    │
      │      投递到 recover_req_queue_                            │
      │                                                           │
      │── 4. RecoverWorker 启动, 调用 db->SnapshotRecover         │
      │      设置状态机标记为 Db::kRecover (阻止读写)             │
      │                                                           │
      │── 5. 通过 TinyRPC 发起 SnapshotRecoverReq ───────────────>│
      │                                                           │── 6. 提取业务 DB 全量快照
      │                                                           │      (包括 max_apply_entry)
      │<── 7. SnapshotRecoverRsp (返回全量快照与最新 Entry) ──────│
      │                                                           │
      │── 8. 本地原子覆写业务 DB                                  │
      │── 9. 将 max_cont_chosen_entry 重置为最新 Entry            │
      │── 10. 解除 kRecover 状态, 完全满血复活！                  │
```

#### 核心源码解析（`certain/default/db_impl.cc`）：
```40:87:certain/default/db_impl.cc
int DbImpl::SnapshotRecover(uint64_t entity_id, uint32_t start_acceptor_id,
                            uint64_t* max_committed_entry) {
  // Step 1: 加锁并将该 Entity 标记为 kRecover (只读且挂起常规更新)
  {
    certain::DbEntityLock lock(this, entity_id);
    int ret = Shard(entity_id).Set(entity_id, -1u, 0, certain::Db::kRecover);
    assert(ret == 0);
  }

  // Step 2: 通过 Route 寻找健康 Peer 的网络地址
  auto route = certain::Certain::GetRoute();
  ...
  // Step 3: 通过 TinyRPC 向健康节点发起快照拉取请求
  certain::TinyClient client(peer_addr);
  certain::TinyController controller;
  certain::SnapshotRecoverReq req;
  req.set_entity_id(entity_id);
  certain::SnapshotRecoverRsp rsp;
  client.SnapshotRecover(&controller, &req, &rsp, nullptr);

  // Step 4: 将拉取到的全量状态机数据导入本地，并更新进度游标
  *max_committed_entry = rsp.max_apply_entry();
  uint32_t crc32 = *reinterpret_cast<const uint32_t*>(rsp.data().data());
  {
    certain::DbEntityLock lock(this, entity_id);
    Shard(entity_id).Set(entity_id, *max_committed_entry, crc32, certain::Db::kNormal);
  }
  return 0;
}
```

#### 恢复总结：
通过快照恢复，落后节点直接将状态机同步到了最新健康节点的 `max_apply_entry`，并继承了最新的数据与 CRC 校验码。
原本缺失且已被物理删除的远古 PLog 被直接**越过**，节点无缝重返集群共识，重新具备承载读写的能力！
这一优雅的双层机制（PLog 增量追平 + DB 全量快照兜底），构成了 PaxosStore 高可用自愈体系的物理基石。
