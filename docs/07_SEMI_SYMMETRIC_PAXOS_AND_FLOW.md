# 07. 半对称消息协议与端到端 Paxos 交互流程

在经典的 Paxos 实现中，协议通常需要定义至少 4 种独立类型的报文：
- `PrepareReq` / `PrepareRsp`
- `AcceptReq` / `AcceptRsp`

此外，如果考虑到日志学习（Learn）、心跳保活和状态对齐，报文类型往往多达十余种。这不仅增加了代码维护复杂度，也在状态机中引入了繁琐的分支判断。

Certain 创造性地提出了**半对称消息（Semi-Symmetric Paxos Message）协议**，将所有的提案、承诺、接受、应答与状态同步高度统一为同一种数据载荷。

---

## 1. 半对称协议（Semi-Symmetric Message）核心思想

在 `certain/proto/certain.proto` 中，核心网络消息仅有这一个结构：

```protobuf
message PaxosMsg {
  optional CmdHeader header = 1;

  optional uint32 local_acceptor_id = 2;
  optional uint32 peer_acceptor_id = 3;

  optional EntryRecord local_entry_record = 4;
  optional EntryRecord peer_entry_record = 5;

  optional uint64 max_chosen_entry = 6;

  optional bool check_empty = 7;
  optional bool catchup = 8;
}
```

### 1.1 什么是“半对称”？视角反转的艺术
任何一条发送出去的 `PaxosMsg` 都携带两个核心记录：
1. **`local_entry_record`**：**发送方（Local）眼中自己当前的最新状态**（包括自身的 `prepared_num`、`promised_num`、`accepted_num`、`value` 和 `chosen` 标记）；
2. **`peer_entry_record`**：**发送方所了解的对端（Peer）最后已知状态**。

当这包数据经由网络到达接收方时，在 `EntityHelper::HandlePaxosCmd` 中，做的第一件事就是**视角对调（`SwitchToLocalView`）**：

```cpp
bool PaxosCmd::SwitchToLocalView(uint32_t local_acceptor_id) {
  if (local_acceptor_id != peer_acceptor_id()) {
    return false;
  }
  // 视角对调：本地变成了对方眼中的 Peer，对方变成了本地眼中的 Peer
  std::swap(local_acceptor_id_, peer_acceptor_id_);
  std::swap(local_entry_record_, peer_entry_record_);
  return true;
}
```

```
[ 发送方 Node A (id: 0) ]                         [ 接收方 Node B (id: 1) ]
local_acceptor_id = 0                             
peer_acceptor_id  = 1                             
local_entry_record = A 自身状态                   
peer_entry_record  = A 眼中 B 的状态              
        │                                                 │
        │─────────── 发送 PaxosMsg (跨网络) ─────────────>│
                                                          │
                                            SwitchToLocalView(1):
                                            local_acceptor_id = 1 (本地变为 B)
                                            peer_acceptor_id  = 0 (对端变为 A)
                                            local_entry_record = A 眼中 B 的状态
                                            peer_entry_record  = A 自身状态
```

### 1.2 为什么这种设计极其强大？
1. **全对称的信息交换**：
   接收方 Node B 收到消息后，拿自己的当前实际记录与消息里的 `local_entry_record`（即 A 以为的 B 的状态）比对：
   - 如果 A 眼中的状态比 B 当前的实际状态旧，说明 A 还没收到 B 最新的回包；
   - 拿消息里的 `peer_entry_record`（即 A 自身的状态）更新本地状态机：`machine->Update(peer_acceptor_id, pcmd->peer_entry_record())`；
2. **请求与应答的高度统一**：
   在 Certain 中，**根本没有显式的“Ack/Response”概念**！
   如果 B 更新完本地状态机后，发现自己的本地记录比 A 眼中的记录更新（`machine->IsRecordNewer(...)`），B 只需要**将更新后的状态机记录打包打回给 A**。这条回发的消息对 B 来说是一次“状态同步”，对 A 来说就是自然的“Promise/Accepted 投票应答”！
3. **天然抗丢包与乱序**：
   哪怕网络严重丢包或报文乱序重放，由于每条消息都携带双方的全部版本编号（`prepared/promised/accepted`），状态机调用 `IsRecordNewer` 自动幂等忽略过期包，完全不会产生协议状态不一致。

---

## 2. A/B/C 三节点集群端到端 Paxos 流程全链路深度拆解

假设现有集群由 A（Node 0）、B（Node 1）、C（Node 2）三节点组成，多数派判定阈值为 $3/2 + 1 = 2$。
客户端连接到 **Node A**，发起对 `entity_id = 30001, entry = 1` 的写入操作（`value = "hello"`）。

以下是源码级别的完整端到端生命周期跟踪：

```
[Client]        [Node A (Proposer/Acceptor 0)]           [Node B (Acceptor 1)]     [Node C (Acceptor 2)]
   │                            │                                  │                         │
   │── 1. Write(E=30001, e=1) ─>│                                  │                         │
   │   (进入 TinyServer/SyncWait)│                                  │                         │
   │                            │── 2. machine->Promise()          │                         │
   │                            │      生成提案号 pn=4              │                         │
   │                            │── 3. StoreEntryInfo (写 Plog)    │                         │
   │                            │<── 4. Plog 落盘回调               │                         │
   │                            │── 5. Broadcast 半对称 PaxosMsg ─>│                         │
   │                            │      (带 local=4, peer=0)        │────────────────────────>│
   │                            │                                  │── 6. Update 状态机      │ (同 Node B)
   │                            │                                  │      落盘 Promised=4     │
   │                            │<── 7. 回发状态 (local=4) ─────────│                         │
   │                            │                                  │                         │
   │                            │── 8. 收到多数派 Promise          │                         │
   │                            │      状态机跃迁为 kMajorityPromise│                         │
   │                            │── 9. machine->Accept("hello")    │                         │
   │                            │      状态机跃迁为 kAcceptLocal    │                         │
   │                            │── 10. StoreEntryInfo (写 Plog)   │                         │
   │                            │<── 11. Plog 落盘回调              │                         │
   │                            │── 12. Broadcast Accept 报文 ────>│                         │
   │                            │                                  │────────────────────────>│
   │                            │                                  │── 13. Update 状态机     │
   │                            │                                  │       落盘 Accepted=4   │
   │                            │<── 14. 回发状态 (Accepted=4) ────│                         │
   │                            │                                  │                         │
   │                            │── 15. 收到多数派 Accept          │                         │
   │                            │       状态机跃迁为 kChosen       │                         │
   │                            │── 16. StoreEntryInfo (写 Plog)   │                         │
   │                            │<── 17. Plog 落盘回调              │                         │
   │                            │── 18. 异步投递 DbWorker  Commit   │                         │
   │                            │── 19. 唤醒协程, 返回 Client 0 ───│                         │
   │<── RetCode 0 (写入成功) ───│                                  │                         │
```

### 步骤详解与关键代码行号：

#### 阶段一：本地 Prepare 与持久化（Phase 1a）
1. **客户端请求接入**：`TinyServiceImpl::Write` 接收请求，调用 `Certain::Write`，进入 `SyncWait` 挂起协程并压入 `user_req_queues_`；
2. **`EntityWorker` 调度**：`EntityHelper::HandleWriteCmd` 执行：
   ```cpp
   // certain/src/entity_helper.cc 第 487 行
   int ret = machine->Promise(pre_auth);
   ```
   计算提案号：公式计算得到当前节点提案编号 $pn = 4$（落入 $> 3$ 的无租约编号区间）。
   状态机由 `kNormal` 跃迁为 `kPromiseLocal`；
3. **Plog 优先落盘**：
   调用 `StoreEntryInfo(info)` 将带有 `promised_num = 4` 的记录投递给 `PlogWorker`。在 `PlogWorker` 调用 RocksDB 刷盘完成前，**绝不向网络发出任何字节**！

#### 阶段二：广播半对称报文与收集承诺（Phase 1b）
4. **Plog 刷盘回调**：`PlogWorker` 将回包放入 `plog_rsp_queue_`，`EntityHelper::HandleSetFromPlog` 被触发，设置 `info->broadcast = true` 并调用 `Broadcast(info)`；
5. **网络分发**：`MsgWorker` 将报文（`PaxosMsg`：`local_entry_record.promised_num = 4`）通过 TCP 长连接推送到 Node B 和 Node C；
6. **对端接收与更新**：
   Node B 的 `MsgWorker` 收到数据包，转换为 `PaxosCmd`，推给 Node B 的 `EntityWorker`：
   - 调用 `pcmd->SwitchToLocalView(1)`；
   - 在 `UpdateMachineByPaxosCmd` 中，调用 `machine->Update(peer_acceptor_id = 0, peer_record)`；
   - Node B 发现 Node A 的 `promised_num = 4 >` 自己本地的承诺值，于是顺从地将本地 `promised_num` 更新为 4，状态机进入 `kPromiseRemote`；
   - Node B 调用 `StoreEntryInfo` 将承诺持久化到自己的 RocksDB；
   - 刷盘后，Node B 将自己更新后的记录作为回包打回给 Node A。

#### 阶段三：多数派承诺与提案值生成（Phase 2a: Accept）
7. **收集承诺**：
   Node A 收到 Node B 的回包，调用 `machine->Update`。
   在 `EntryStateMachine::CalcEntryState()` 中：
   ```cpp
   // certain/src/entry_state.cc 第 101-110 行
   uint32_t promised_count = 0;
   for (size_t i = 0; i < acceptor_num_; ++i) {
     if (entry_records_[i].promised_num() == record.promised_num()) {
       promised_count++;
     }
   }
   if (promised_count >= acceptor_num_ / 2 + 1) {
     entry_state_ = EntryState::kMajorityPromise;
   }
   ```
   节点 A 和节点 B 均承诺了 4，$2 \ge 2$，达到多数派！状态机瞬间跃迁为 **`kMajorityPromise`**！
8. **生成提案值并落盘**：
   在 `EntityHelper::UpdateMachineByPaxosCmd` 第 753 行：
   检测到状态变为 `kMajorityPromise`，立即触发：
   ```cpp
   ret = machine->Accept(client_cmd->value(), client_cmd->value_id(),
                         client_cmd->uuids(), &prepared_value_accepted);
   ```
   遵循 Lamport P2c 不变式：若无历史接受值，提议客户端的新值 `"hello"`，状态跃迁为 `kAcceptLocal`，随后再次写入 Plog。

#### 阶段四：多数派接受与选定（Phase 2b & Chosen）
9. **广播 Accept**：Plog 刷盘完成后，广播携带最新 Accept 数据的 `PaxosMsg`；
10. **对端持久化 Accepted**：Node B 收到后更新本地状态为 `kAcceptRemote`，落盘并回复；
11. **达成 Chosen**：
    Node A 收到 Node B 的 Accept 回复，`CalcEntryState()` 计算：
    ```cpp
    if (accepted_count >= acceptor_num_ / 2 + 1) {
      entry_state_ = EntryState::kChosen;
      entry_records_[local_acceptor_id_].set_chosen(true);
    }
    ```
    状态机权威跃迁至 **`kChosen`**！
12. **应用到状态机与返回客户端**：
    - `StoreEntryInfo` 持久化最终的 Chosen 状态；
    - `DbWorker::GoToDbReqQueue` 异步触发应用层 Commit；
    - `EntityWorker::ReplyClientCmd` 触发 `LibcoNotifyHelper::Notify`，客户端挂起的协程瞬间被唤醒，RPC 返回 `ret = 0`，全流程闭环！

---

## 3. 预授权优化（Pre-Authorization / Lease）代码级深度拆解

在上一轮问答中，我们已经了解到 Pre-Auth 能够将 2-RTT 削减为 1-RTT。现在，我们结合源码行号深度挖掘其代码实现。

### 3.1 Pre-Auth 产生的契机
在 `certain/src/entity_helper.cc` 中，当某个 entry 达成 Chosen 时：

```1033:1038:certain/src/entity_helper.cc
  if (machine->HasAcceptedMyProposal(entity_info->local_acceptor_id) &&
      (entity_info->pre_auth_entry == kInvalidEntry ||
       entity_info->pre_auth_entry < info->entry)) {
    entity_info->pre_auth_entry = info->entry;
  }
```
**关键触发条件**：
只有当达成 Chosen 的提案**正是本节点自己发起的提案**（`HasAcceptedMyProposal` 为 true）时，本地才会将 `entity_info->pre_auth_entry` 推进到该 `entry`。
这相当于本节点自动获得了**对下一个连续 Entry（`entry + 1`）的 Proposer Lease（租约）**！

---

### 3.2 快速路径生效的关键代码：`IsLocalAcceptable()`

当业务接着写入下一个 entry（`entry == pre_auth_entry + 1`）时：

1. **计算 Pre-Auth 提案编号**：
   在 `EntityHelper::HandleWriteCmd` 中：
   ```cpp
   bool pre_auth = false;
   if (options_->enable_pre_auth() &&
       entity_info->pre_auth_entry != kInvalidEntry) {
     pre_auth = (entity_info->pre_auth_entry + 1 == entry);
   }
   int ret = machine->Promise(pre_auth);
   ```
   传入 `pre_auth = true`。
   在 `EntryStateMachine::Promise` 中：
   ```cpp
   pn = (pn + n - 1) / n * n + local_acceptor_id_ + 1;
   if (!pre_auth && pn <= n) {
     pn += n;
   }
   ```
   由于 `pre_auth == true`，代码**跳过了 `pn += n`**！
   在 3 节点集群下（$n=3$），Node 0 计算得到的编号是 $0 + 1 = 1 \le 3$！

2. **绕过多数派 Promise 判定**：
   在 `HandleWriteCmd` 中紧接着检查：
   ```cpp
   if (!machine->IsLocalAcceptable()) {
     // 如果不可直接 Accept，必须落盘并广播 Prepare (经典 2-RTT 慢路径)
     ...
     return kRetCodeWaitBroadcast;
   }

   // 奇迹发生：直接执行 Accept 并广播！(1-RTT 快路径)
   ret = machine->Accept(client_cmd->value(), ...);
   ```
   进入 `IsLocalAcceptable()` 源码：
   ```257:263:certain/src/entry_state.cc
   bool EntryStateMachine::IsLocalAcceptable() const {
     if (entry_state_ != EntryState::kMajorityPromise &&
         !(entry_state_ == EntryState::kPromiseLocal &&
           GetLocalPromisedNum() <= acceptor_num_)) {
       return false;
     }
     return true;
   }
   ```
   **核心神来之笔**：
   由于 `GetLocalPromisedNum() <= acceptor_num_`（即 $1 \le 3$ 成立），代码直接返回 **`true`**！
   系统完全免去了与 Peer 的 Phase 1 网络往返，直接在本地产生提案值，并把 Phase 2 Accept 广播出去！
   **结果**：仅需 1 次网络往返，直接达成 Chosen，实现极致写性能！

---

## 4. 全新现代版协议交互矢量流程图

为了让你拥有直观的物理级全局时序感，我们参考经典架构并结合当前开源版本的最新代码（Wrapper、EntityWorker、PlogWorker、MsgWorker、DbWorker、半对称消息交互），绘制了高精度的交互流程矢量图：

📄 **文件位置**：[**`docs/paxosstore-modern-protocol-process.svg`**](./docs/paxosstore-modern-protocol-process.svg)

该矢量图采用标准 SVG 格式，清晰标明了：
1. Client 协程通过 `LibcoNotifyHelper` 挂起与被唤醒的过程；
2. Proposer 端 `EntityWorker` $\to$ `PlogWorker` $\to$ `MsgWorker` 队列交互；
3. 跨网络的半对称 `PaxosMsg` 报文传输与 `SwitchToLocalView`；
4. Acceptor 端接收、状态机推进与 Plog 刷盘；
5. 多数派决策达成 Chosen 后向 `DbWorker` 异步 Commit 业务数据库的全生命周期。
