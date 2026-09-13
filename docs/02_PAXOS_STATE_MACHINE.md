# 02. Paxos 状态机源码深度剖析

在 Leslie Lamport 的奠基之作 *《Paxos Made Simple》* 中，Paxos 协议以其理论上的极简性著称。然而在工业级实现中，如何将抽象的 Phase 1/Phase 2 映射为高效、无锁、支持流水线操作的状态机，是各大分布式存储团队的核心机密。

本篇文档将深入剖析 Certain 的共识心脏 —— **`EntryStateMachine`**（位于 `certain/src/entry_state.h` 和 `certain/src/entry_state.cc`）。

---

## 1. 理论映射：Paxos Made Simple 与 Certain 状态机对照

在 Lamport 经典 Paxos 算法中，决议一个槽位分为两个阶段：

| 理论阶段 (Lamport) | 协议动作与语义 | Certain 状态机对应状态 |
| :--- | :--- | :--- |
| **初始态** | 槽位为空，尚无任何提案活动 | `EntryState::kNormal` (0) |
| **Phase 1a (Prepare)** | Proposer 选择编号 $n$，发送 Prepare 请求 | 本地调用 `Promise()`，提升 `prepared_num` |
| **Phase 1b (Promise)** | Acceptor 承诺拒绝更小编号，返回已接受的最大提案 $(n_a, v_a)$ | 本地处于 `kPromiseLocal` (1)；对端处于 `kPromiseRemote` (2) |
| **Majority Promise** | Proposer 获得多数派 Acceptor 的承诺 | `EntryState::kMajorityPromise` (3) |
| **Phase 2a (Accept)** | Proposer 决定提案值（遵循历史最高值规则），发起广播 | 本地调用 `Accept()`，处于 `kAcceptLocal` (5) |
| **Phase 2b (Accepted)**| Acceptor 接受该提案，持久化记录 | 对端处于 `kAcceptRemote` (4) |
| **Chosen** | 多数派 Acceptor 接受了相同提案值 | `EntryState::kChosen` (6) |

### 1.1 状态机枚举定义

在 `certain/src/entry_state.h` 中：

```10:20:certain/src/entry_state.h
enum struct EntryState {
  kNormal = 0,
  kPromiseLocal = 1,
  kPromiseRemote = 2,
  kMajorityPromise = 3,
  kAcceptRemote = 4,
  kAcceptLocal = 5,
  kChosen = 6
};
```

每个 `EntryStateMachine` 实例维护一个大小为 `kMaxAcceptorNum`（默认最大支持 5 副本）的数组：
```cpp
EntryRecord entry_records_[kMaxAcceptorNum];
```
本节点直接在内存中全量维护**集群所有副本在该槽位上的已知投票快照**，每次收到 Peer 报文或本地发生修改时，通过 `CalcEntryState()` 重新评估全局收敛状态。

---

## 2. 状态机迁移全景图（State Transition Diagram）

```
                  ┌─────────────────┐
                  │     kNormal     │
                  └────────┬────────┘
                           │
       ┌───────────────────┴───────────────────┐
       │ 本地发起写 (Promise)                   │ 收到 Peer Prepare
       ▼                                       ▼
┌──────────────┐                       ┌───────────────┐
│kPromiseLocal │                       │kPromiseRemote │
└──────┬───────┘                       └───────┬───────┘
       │                                       │
       │ 收到多数派 Promise                     │ 收到更高编号 Accept
       ▼                                       ▼
┌──────────────────┐                   ┌───────────────┐
│ kMajorityPromise │                   │ kAcceptRemote │
└──────┬───────────┘                   └───────┬───────┘
       │                                       │
       │ 本地生成提案并持久化 (Accept)           │
       ▼                                       │
┌──────────────┐                               │
│ kAcceptLocal │                               │
└──────┬───────┘                               │
       │                                       │
       │ 收到多数派 Accept                      │ 收到 Chosen 报文
       ▼                                       ▼
┌──────────────────────────────────────────────────────┐
│                       kChosen                        │
│             (共识达成，允许提交至业务 DB)             │
└──────────────────────────────────────────────────────┘
```

---

## 3. 提议编号（Proposal Number）生成算法与数学证明

在分布式环境下，为了保证全局提案编号单调自增且**副本间绝对不冲突**，Lamport 建议将副本 ID 编码至提案编号中。

在 `certain/src/entry_state.cc` 的 `EntryStateMachine::Promise` 中：

```233:255:certain/src/entry_state.cc
int EntryStateMachine::Promise(bool pre_auth) {
  EntryRecord& record = entry_records_[local_acceptor_id_];
  uint32_t pn = record.promised_num();
  uint32_t n = acceptor_num_;

  pn = (pn + n - 1) / n * n + local_acceptor_id_ + 1;
  if (!pre_auth && pn <= n) {
    // proposal number not large then n is use for pre-auth only.
    pn += n;
  }

  record.set_prepared_num(pn);
  record.set_promised_num(pn);
  CalcEntryState();

  if (entry_state_ != EntryState::kPromiseLocal) {
    return kRetCodeInvalidEntryState;
  }

  return 0;
}
```

### 3.1 数学公式与无碰撞证明
设集群总副本数为 $n$（如 3），当前节点的编号为 `local_acceptor_id`（记为 $id$，满足 $0 \le id < n$），前序记录的承诺编号为 $pn_{old}$。
Certain 采用如下公式生成新提案编号 $pn_{new}$：
$$pn_{new} = \left\lfloor \frac{pn_{old} + n - 1}{n} \right\rfloor \cdot n + id + 1$$

**证明 1：严格单调递增性**
若 $pn_{old} \ge 1$，则 $\frac{pn_{old} + n - 1}{n} \ge 1$。
因为 $id + 1 \ge 1$，乘法部分为向上圆整到 $n$ 的倍数，加上 $id + 1$ 后必然满足：
$$pn_{new} > pn_{old}$$
这保证了 Proposer 的每一次重试都能产生严格大于前一次的编号。

**证明 2：副本间绝对无碰撞**
对任意节点 $A$ 和 $B$（$id_A \ne id_B$）：
$$pn_A \pmod n = (id_A + 1) \pmod n$$
$$pn_B \pmod n = (id_B + 1) \pmod n$$
由于 $0 \le id_A, id_B < n$，必有 $pn_A \pmod n \ne pn_B \pmod n$。
**结论**：不同节点生成的提案编号在代数上永远不相等！

**证明 3：Pre-Auth 编号空间隔离**
代码中有关键条件：
```cpp
if (!pre_auth && pn <= n) {
  pn += n;
}
```
当节点**未获得 Pre-Auth（预授权）**时，如果算出的 $pn \le n$，则强制加上 $n$。
这意味着：
- 区间 $[1, n]$ 的提案编号被**严格保留**给持有 Pre-Auth 的节点使用；
- 正常的无租约提案编号直接从 $n + 1$ 开始起步。

---

## 4. Phase 2a：提案值选择安全性约束（Value Selection Rule）

Paxos 算法之所以能保证绝对一致性（Safety），其核心支柱是 Lamport 在 *Paxos Made Simple* Section 2.2 中指出的不变式：
> **Invariance P2c**：对任意 $v$ 和 $n$，如果编号为 $n$ 的提案具有值 $v$，那么存在一个多数派集合 $S$，其中要么没有 Acceptor 曾接受过小于 $n$ 的提案，要么在被 $S$ 接受的最高编号提案中，其值也必须是 $v$。

在 `certain/src/entry_state.cc` 的 `Accept` 方法中，代码极其严谨地践行了这一不变式：

```274:315:certain/src/entry_state.cc
int EntryStateMachine::Accept(const std::string& value, uint64_t value_id,
                              const std::vector<uint64_t>& uuids,
                              bool* prepared_value_accepted) {
  if (entry_state_ != EntryState::kMajorityPromise &&
      !(entry_state_ == EntryState::kPromiseLocal &&
        GetLocalPromisedNum() <= acceptor_num_)) {
    return kRetCodeInvalidEntryState;
  }

  EntryRecord& record = entry_records_[local_acceptor_id_];
  uint32_t promised_num = record.promised_num();
  assert(promised_num > 0);

  // Select the value with max accepted_num.
  uint32_t selected = 0;
  uint32_t max_accepted_num = entry_records_[0].accepted_num();
  for (uint32_t i = 1; i < acceptor_num_; ++i) {
    if (max_accepted_num < entry_records_[i].accepted_num()) {
      selected = i;
      max_accepted_num = entry_records_[i].accepted_num();
    }
  }

  if (max_accepted_num > 0) {
    const auto& selected_record = entry_records_[selected];
    record.set_accepted_num(promised_num);
    record.set_value(selected_record.value());
    record.set_value_id(selected_record.value_id());
    *record.mutable_uuids() = {selected_record.uuids().begin(),
                               selected_record.uuids().end()};
    *prepared_value_accepted = false;
  } else {
    record.set_accepted_num(promised_num);
    record.set_value(value);
    record.set_value_id(value_id);
    *record.mutable_uuids() = {uuids.begin(), uuids.end()};
    *prepared_value_accepted = true;
  }

  CalcEntryState();
  assert(entry_state_ == EntryState::kAcceptLocal);

  return 0;
}
```

### 关键代码逻辑深度剖析：
1. **历史值扫描**：
   遍历所有副本的 `accepted_num`，找出其中的最大值 `max_accepted_num`；
2. **强制继承历史值**（`max_accepted_num > 0`）：
   如果集群中有任何副本曾经在 Phase 2 接受过值，Proposer **必须强制放弃当前客户端提交的新数据**，将自己的提案值锁定为历史遗留值，并将 `*prepared_value_accepted = false`。
   在调用层（`EntityHelper::UpdateMachineByPaxosCmd`），当检测到 `!prepared_value_accepted` 时，会直接向客户端返回 `kRetCodeStateAcceptFailed`，通知客户端重新重试（客户端 Replay 后将看到历史写入，并在下一 entry 写入）。
3. **接受自身新值**（`max_accepted_num == 0`）：
   只有当多数派都未曾接受过任何历史值时，Proposer 才可以安全地提议本次客户端写入的业务数据（`prepared_value_accepted = true`）。

---

## 5. Multi-Paxos 1-RTT 快速路径：Pre-Authorization（租约）

经典 Paxos 写入一个槽位需要 2 个完整往返（2-RTT）：
- RTT 1: Phase 1a (Prepare) $\to$ Phase 1b (Promise)
- RTT 2: Phase 2a (Accept) $\to$ Phase 2b (Accepted)

在跨数据中心部署下，2 个 RTT 的网络延迟通常会超过 40ms，无法满足微信业务低延时需求。

### 5.1 租约递推原理
PaxosStore 论文（VLDB 2017 Section 3.3）提出了 **Pre-Authorization（预授权）** 机制：
当节点 $A$ 成功 Chosen 了连续的第 $i$ 个 entry 时，该节点自动获得对下一个槽位 $i+1$ 的 Pre-Auth 权（记录在 `entity_info->pre_auth_entry = i`）。
在写入 entry $i+1$ 时：
1. 调用 `Promise(pre_auth = true)`：
   生成的提案编号 $pn = id + 1 \le n$（落入预留的 Pre-Auth 编号区间）；
2. 检查 `IsLocalAcceptable()`：
```cpp
bool EntryStateMachine::IsLocalAcceptable() const {
  if (entry_state_ != EntryState::kMajorityPromise &&
      !(entry_state_ == EntryState::kPromiseLocal &&
        GetLocalPromisedNum() <= acceptor_num_)) {
    return false;
  }
  return true;
}
```
因为满足 `GetLocalPromisedNum() <= acceptor_num_`，即使当前状态机仅停留在 `kPromiseLocal`（未与 Peer 发生任何网络交互），`IsLocalAcceptable()` 依然直接返回 **`true`**！
3. **跳过 Phase 1 网络交互**：
   节点立刻在本地执行 `Accept(...)`，并将提案打包为 Accept 报文直接广播给 Peer。
4. **效果**：仅需 Phase 2 的 1 个 RTT，多数派节点返回 Accepted，立即达成 Chosen！延迟减半！

---

## 6. 大 Value 传输优化：`has_value_id_only`

在微信业务中，朋友圈图片元数据、群聊天记录等可能达到数十 KB 甚至数 MB。如果在 Paxos 投票重传、状态同步中频繁拷贝大 payload，会导致严重的带宽和 CPU 消耗。

Certain 提出了 `value_id` 解耦方案：
- 本地 Promise 的编号天然全局唯一，直接作为该 Entry 写入数据的 `value_id`；
- 在报文交互中，若节点判断对端已知或本地已有该数据，设置 `has_value_id_only = true`，不携带真实的 `value` 字节；
- 接收端在持久化前，通过 `GetByValueId` 从本地缓存或先前接收的副本中恢复真实负载：

```20:38:certain/src/entry_state.cc
void EntryStateMachine::RestoreValueInRecord(EntryRecord& record) {
  assert(record.has_value_id_only());
  record.set_has_value_id_only(false);

  std::string value;
  std::vector<uint64_t> uuids;
  int ret = GetByValueId(record.value_id(), value, uuids);
  if (ret != 0) {
    CERTAIN_LOG_FATAL("E(%lu, %lu) st: %s vid %lu not found", entity_id_,
                      entry_, ToString().c_str(), record.value_id());
    // If GetByValueId failed, it's equals to drop messages.
    record.clear_chosen();
    record.clear_value();
    record.clear_uuids();
    return;
  }
  record.set_value(value);
  *record.mutable_uuids() = {uuids.begin(), uuids.end()};
}
```
通过元数据与负载的分离，Certain 实现了超高的网络传输效率与吞吐量。
