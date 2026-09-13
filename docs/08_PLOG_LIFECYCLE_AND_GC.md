# 08. 协议日志 PLog 的生命周期、批量读写与 GC 机制

在 Paxos 共识引擎中，**PLog（Paxos Log）**是决议状态持久化的唯一真理来源（Source of Truth）。一旦发生断电、崩溃或集群脑裂，所有状态机重建、数据回放与节点自愈全部依赖 PLog。

本篇文档将结合 `certain/src/plog_worker.cc`、`certain/default/plog_impl.cc` 以及 RocksDB 底层机制，深度剖析 PLog 的整体读写流水线、组提交与日志回收（GC）机制。

---

## 1. PLog 整体架构与读写分离设计

为了防止高并发写入与繁重的历史日志读取相互阻塞，Certain 将 PLog 划分为两个完全隔离的 Worker 线程池：
1. **`PlogWorker`（默认 16 个线程）**：**专职写流水线**。负责接收 `EntityWorker` 发起的 Promise、Accept、Chosen 状态持久化请求；
2. **`PlogReadonlyWorker`（默认 32 个线程）**：**专职读流水线**。负责实体冷启动时的 `LoadMaxEntry`、历史数据扫描 `RangeGetRecord` 以及数据补齐。

```
                     ┌───────────────────────────────┐
                     │         EntityWorker          │
                     └───────┬───────────────┬───────┘
                             │               │
        写请求 (StoreEntryInfo)│               │ 读/加载 (LoadEntryInfo)
                             ▼               ▼
                 ┌────────────────┐ ┌─────────────────────────┐
                 │  plog_req_queue│ │ plog_readonly_req_queue │
                 └───────┬────────┘ └────────────┬────────────┘
                         │                       │
                         ▼                       ▼
                 ┌────────────────┐ ┌─────────────────────────┐
                 │   PlogWorker   │ │   PlogReadonlyWorker    │
                 │   (组提交写入)  │ │      (并发只读)        │
                 └───────┬────────┘ └────────────┬────────────┘
                         │                       │
                         └───────────┬───────────┘
                                     ▼
                            ┌─────────────────┐
                            │    PlogImpl     │
                            │ (RocksDB 实例池) │
                            └─────────────────┘
```

---

## 2. PLog 写入完整流程与批量组提交（Group Commit）

### 2.1 写入入口：`EntityHelper::StoreEntryInfo`
在 `certain/src/entity_helper.cc` 中，当状态机发生 Promise、Accept 或 Chosen 变更时，必须调用 `StoreEntryInfo`：

```cpp
bool EntityHelper::StoreEntryInfo(EntryInfo* info) {
  assert(!info->uncertain);
  info->uncertain = true; // 标记该槽位状态正在落盘，处于未决状态

  auto pcmd = std::make_unique<PaxosCmd>(kCmdPaxos);
  pcmd->set_plog_set_record(true);
  pcmd->set_entity_id(info->entity_info->entity_id);
  pcmd->set_entry(info->entry);
  pcmd->set_local_entry_record(info->machine->GetEntryRecord(local_acceptor_id));

  // 投递至 PlogReqQueue
  return PlogWorker::GoToPlogReqQueue(pcmd) == 0;
}
```

### 2.2 `PlogWorker` 的批量采集与分片聚合
在 `certain/src/plog_worker.cc` 的 `Run()` 方法中：

```47:73:certain/src/plog_worker.cc
void PlogWorker::Run() {
  std::vector<std::unique_ptr<PaxosCmd>> cmds;
  while (!exit_flag()) {
    while (cmds.size() < options_->max_plog_batch_size()) {
      std::unique_ptr<PaxosCmd> cmd;
      int ret = plog_req_queue_->PopByOneThread(&cmd);
      if (ret != 0 || cmd == nullptr) {
        break;
      }
      // write only
      assert(cmd->cmd_id() == kCmdPaxos);

      cmds.push_back(std::move(cmd));
      assert(cmds.back()->plog_set_record());
    }

    if (cmds.empty()) {
      poll(NULL, 0, 1);
      continue;
    }

    SetRecord(cmds);
    for (auto& cmd : cmds) {
      GoToPlogRspQueue(cmd);
    }
    cmds.clear();
  }
}
```

1. **批量拉取（Batching）**：
   单线程从 `plog_req_queue_` 中一次性取出最多 `max_plog_batch_size()`（默认数十至上百条）请求；
2. **多 DB 实例哈希分组**：
   在 `SetRecord` 中，根据 `HashId(entity_id)` 将请求分配给对应的底层 RocksDB 实例：
   ```cpp
   std::unordered_map<uint32_t, std::pair<std::vector<PaxosCmd*>, std::vector<Plog::Record>>> records_groups;
   ```
3. **调用 RocksDB WriteBatch 原子写入**：
   在 `certain/default/plog_impl.cc` 中：
   ```cpp
   int PlogImpl::MultiSetRecords(uint32_t hash_id, const std::vector<Plog::Record>& records) {
     dbtype::WriteBatch batch;
     for (const auto& rec : records) {
       auto key = EntryKey(rec.entity_id, rec.entry, 0);
       batch.Put(key.Serialize(), rec.record);
     }
     static dbtype::WriteOptions options;
     return DB(hash_id)->Write(options, &batch).ok() ? 0 : kImplPlogSetErr;
   }
   ```
   **物理优势**：将上百个来自不同实体的离散写请求，在内存中归并为一个 `WriteBatch` 顺序追加到底层 WAL 和 MemTable，单次磁盘 fsync 损耗被上百个并发写平摊，写吞吐大幅提升！

### 2.3 唤醒与状态机推进：`HandleSetFromPlog`
当刷盘完成后，`PlogWorker` 将命令推回 `plog_rsp_queue_`：
1. `info->uncertain = false`（解除未决状态）；
2. 校验 `machine->entry_state()`：若已是 `kChosen`，顺手将确认信息同步给业务；
3. 如果标记了 `info->broadcast = true`，立刻触发 `Broadcast(info)` 向集群其他节点发送 Paxos 网络消息！

---

## 3. PLog 异步读取与加载流程

读取请求主要发生在冷启动元数据加载或 Catchup 状态追平阶段。

### 3.1 点查与范围查接口
在 `certain/default/plog_impl.cc` 中：
- **`GetRecord(entity_id, entry, &record)`**：
  构造 `EntryKey(entity_id, entry, 0)`，直接调用 `DB->Get`；
- **`RangeGetRecord(entity_id, begin_entry, end_entry, &records)`**：
  利用大端序聚集特性：
  ```cpp
  auto begin_key = EntryKey(entity_id, begin_entry, 0);
  iter->Seek(begin_key.Serialize());
  while (iter->Valid()) {
    auto& key = Parse(iter->key());
    if (key.EntityId() != entity_id || key.Entry() > end_entry) break;
    records->emplace_back(key.Entry(), iter->value().ToString());
    iter->Next();
  }
  ```
  连续范围扫描无需随机点查，利用 RocksDB 的 BlockCache 和预读机制（Read-ahead），毫秒级完成数百个 Entry 的顺序拉取。

---

## 4. PLog 的截断与垃圾回收机制（PLog GC）

在生产环境中，随着时间推移，业务会不断产生海量写入。如果 PLog 只增不减，磁盘空间必将迅速耗尽。

### 4.1 核心理论原则：什么时候 PLog 可以“安全删除”？
> **核心定理**：当且仅当一个 Entry 的提案值已经被**严格连续地应用（Apply）到业务 DB 状态机**，且集群中多数派节点都已经持久化该状态之后，该 Entry 对应的 PLog 才能被安全回收！

如果一个 Entry 已经写入了业务 DB，且即使节点崩溃重启，业务 DB 也能从快照和自身的重放日志中恢复数据，那么共识过程记录它的 `prepared/promised/accepted` 历史便不再需要。

---

### 4.2 RocksDB CompactionFilter 实现机制（`PlogFilter`）

在 `certain/example/task_perf.cc` 中，给出了微信生产环境所采用的标准 PLog GC 机制 —— **基于 RocksDB 的 CompactionFilter**：

```59:97:certain/example/task_perf.cc
class PlogFilter : public dbtype::CompactionFilter {
 public:
  PlogFilter(DbImpl* db_impl) : db_impl_(db_impl) {}
  virtual ~PlogFilter() {}

  virtual bool Filter(int level, const dbtype::Slice& key,
                      const dbtype::Slice& existing_value,
                      std::string* new_value, bool* value_changed) const {
    if (level < 1) {
      // Level 0 暂不删除，保留缓冲
      return false;
    }
    thread_local uint64_t prev_entity_id = 0;
    thread_local uint64_t prev_max_committed_entry = 0;

    uint64_t entity_id = 0;
    uint64_t entry = 0;
    PlogImpl::ParseKey(key, &entity_id, &entry);

    if (entity_id != prev_entity_id) {
      certain::Db::RecoverFlag flag;
      int ret =
          db_impl_->GetStatus(entity_id, &prev_max_committed_entry, &flag);
      if (ret != 0 || flag != certain::Db::kNormal) {
        return false;
      }
      prev_entity_id = entity_id;
    }
    // 关键判断：如果该 entry 已经小于等于业务 DB 提交的进度，直接丢弃删除！
    return entry <= prev_max_committed_entry;
  }

  virtual const char* Name() const { return "plog_filter"; }

 private:
  DbImpl* db_impl_;
};
```

#### 工作原理解密：
1. **零开销后台清理**：
   不需要单独开线程全盘扫表执行 `Delete()`（单独写 Delete 会产生大量的 Tombstone 墓碑标记，恶化读性能）；
2. **结合 RocksDB 压缩（Compaction）**：
   当后台执行 Level 压缩时，`PlogFilter` 会自动被调用；
3. **以 DB 提交游标为基准**：
   提取 Key 中的 `entity_id` 和 `entry`，查询业务 DB：`db_impl_->GetStatus(entity_id, &prev_max_committed_entry)`；
4. **安全丢弃**：
   只要 `entry <= prev_max_committed_entry`，返回 `true`，RocksDB 在写出新的 SST 文件时**直接丢弃该数据**！磁盘空间自动、平滑地得到释放！

---

### 4.3 PLog 被删除后引发的连锁反应：从 Catchup 到 Recover

PLog 的安全删除带来了一个不可避免的极端边缘场景：
1. 集群正常运行，Node A 离线了很长时间（例如关机 3 天）；
2. Node B 和 Node C 达成了成千上万个新的 Entry 共识，并全部 Commit 到了 DB；
3. RocksDB 触发 Compaction，将这些早已提交的旧 PLog 全部通过 `PlogFilter` 物理删除了；
4. **Node A 重启上线**，尝试找 Node B/C 追赶日志（Catchup）；
5. Node B/C 翻查自己的 RocksDB PLog，发现对应区间已经被删了，返回 **`NotFound` (`kImplPlogNotFound`)**！
6. **自愈决裂**：此时基于 PLog 的增量追赶彻底失效！
7. **降级逃生通道（Recover）**：系统自动触发冷备快照恢复（`SnapshotRecover`），从存活节点直接拷贝业务 DB 的状态，彻底重置 Entity 状态机！这正是我们将在下一篇文档深度解析的**快照恢复机制**。
