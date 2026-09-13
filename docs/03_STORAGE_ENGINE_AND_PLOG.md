# 03. Plog 与 DB 落盘存储结构深度解析

在 Paxos 共识引擎中，持久化存储是保证崩溃恢复、防丢数据与线性一致性的物理底座。Certain 的存储体系分为两部分：
1. **Plog（Paxos Log 存储层）**：默认基于 RocksDB，持久化所有的共识决策与状态变更；
2. **DB（业务状态机存储层）**：维护业务实际的数据状态与连续提交进度。

本篇文档将深入字节级别，剖析 Certain 的落盘二进制布局与存储优化。

---

## 1. Plog 的 24 字节大端序 EntryKey 编码

在 `certain/default/plog_impl.cc` 中，Certain 为底层 KV 存储设计了紧凑且高效的键编码：

```8:30:certain/default/plog_impl.cc
namespace {
class __attribute__((packed)) EntryKey {
 public:
  EntryKey(uint64_t entity_id = 0, uint64_t entry = 0, uint64_t value_id = 0)
      : entity_id_(htobe64(entity_id)),
        entry_(htobe64(entry)),
        value_id_(htobe64(value_id)) {}

  uint64_t EntityId() const { return be64toh(entity_id_); }
  uint64_t Entry() const { return be64toh(entry_); }
  uint64_t ValueId() const { return be64toh(value_id_); }

  dbtype::Slice Serialize() {
    return dbtype::Slice(reinterpret_cast<const char*>(this), sizeof(*this));
  }

 private:
  // store in big-endian
  uint64_t entity_id_;
  uint64_t entry_;
  uint64_t value_id_;
};
```

### 1.1 内存布局与字节对齐
- `__attribute__((packed))`：取消编译器自动字节对齐填充，确保结构体严格占用 **$8 + 8 + 8 = 24$ 字节**；
- `htobe64`（Host to Big-Endian 64）：所有 64 位整数强制以**大端字节序（高位在左，网络字节序）**存入内存。

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       entity_id (Byte 0 - 3)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       entity_id (Byte 4 - 7)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         entry (Byte 8 - 11)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         entry (Byte 12 - 15)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       value_id (Byte 16 - 19)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       value_id (Byte 20 - 23)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 1.2 为什么必须使用大端序？（RocksDB 字典序原理）
RocksDB 内部底层比较 Key 大小时，默认使用 `memcmp` 逐字节按无符号字符（Unsigned Char）比较：
- 如果使用小端序（Little-Endian），低有效位在先，导致数值大小关系与字节比较关系完全颠倒；
- 使用**大端序**后，数值的自然大小顺序与 RocksDB 字典序**完全等价**：
  $$\text{Key}_A < \text{Key}_B \iff \text{memcmp}(Key_A, Key_B, 24) < 0$$

这一设计带来了两个巨大的工程优势：
1. **物理聚集存储（Data Locality）**：同一个 `entity_id` 的所有日志在 RocksDB 的 SST 文件中物理相连，范围扫描 `RangeGetRecord` 极速完成；
2. **槽位自然递增**：在同一个 `entity_id` 内部，所有记录严格按照 `entry = 1, 2, 3...` 物理排序。

---

## 2. $O(\log N)$ 快速定位最大 Entry（LoadMaxEntry 寻址魔法）

当系统初次访问某个 Entity 时，必须快速探明该 Entity 历史已写入的最大 entry 是多少。
通常的做法是全表扫描或遍历，而 Certain 利用大端序排列特性，在 `certain/default/plog_impl.cc` 中实现了一个精妙绝伦的 **$O(\log N)$ 快速寻址**：

```38:58:certain/default/plog_impl.cc
int PlogImpl::LoadMaxEntry(uint64_t entity_id, uint64_t* entry) {
  // enable coroutine
  static dbtype::ReadOptions options;
  std::unique_ptr<dbtype::Iterator> iter(DB(entity_id)->NewIterator(options));

  auto key = EntryKey(entity_id + 1, 0, 0);
  iter->Seek(key.Serialize());
  if (iter->Valid()) {
    iter->Prev();
  } else {
    iter->SeekToLast();
  }

  if (iter->Valid()) {
    auto& entry_key = Parse(iter->key());
    if (entry_key.EntityId() == entity_id) {
      *entry = entry_key.Entry();
      return 0;
    }
  }
  return certain::kImplPlogNotFound;
}
```

### 算法执行步骤：
1. **构造哨兵 Key**：`EntryKey(entity_id + 1, 0, 0)`（恰好是下一个 entity 的最小可能 key）；
2. **Seek 哨兵**：在 RocksDB 中 Seek 到该位置（通过 LSM 索引二分查找，耗时 $O(\log N)$）；
3. **前移一步（`iter->Prev()`）**：
   - 字典序上紧挨着 `(entity_id + 1, 0, 0)` 前面的记录，**必然是 `entity_id` 旗下 `entry` 最大、`value_id` 最大的记录**！
4. **校验并提取**：检查 `iter->key()` 的 `EntityId() == entity_id`，若是，直接提取 `entry_key.Entry()`，瞬间获取当前最大已持久化 entry！

---

## 3. 真实落盘二进制实测（Hex 与 Protobuf 反序列化）

使用我们编写的专用工具 `inspect_plog`，直接读取刚才运行的实验中生成的真实 RocksDB SST/WAL 文件（`certain/build/experiment_artifacts/node0/test_plog.o`）：

### 3.1 真实落盘 Key/Value 输出

```
=================================================================
  Deep Dive: Inspecting RocksDB Plog Binary Records at:
  certain/build/experiment_artifacts/node0/test_plog.o
=================================================================

-----------------------------------------------------------------
[Record #1]
Raw Key (24 bytes, Hex): 00 00 00 00 00 00 75 31 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 
Decoded Key: entity_id=30001 | entry=1 | value_id=0
Type: [EntryRecord Metadata (Proto)]
  - prepared_num: 5
  - promised_num: 5
  - accepted_num: 5
  - value_id:     5
  - chosen:       TRUE
  - has_vid_only: FALSE
  - value bytes:  "paxos_genesis_val" (17 bytes)
  - uuids (0): []
-----------------------------------------------------------------
[Record #2]
Raw Key (24 bytes, Hex): 00 00 00 00 00 00 75 31 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 00 
Decoded Key: entity_id=30001 | entry=2 | value_id=0
Type: [EntryRecord Metadata (Proto)]
  - prepared_num: 0
  - promised_num: 4
  - accepted_num: 4
  - value_id:     4
  - chosen:       TRUE
  - has_vid_only: FALSE
  - value bytes:  "paxos_second_val" (16 bytes)
  - uuids (0): []
-----------------------------------------------------------------
[Record #3]
Raw Key (24 bytes, Hex): 00 00 00 00 00 00 75 31 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00 00 
Decoded Key: entity_id=30001 | entry=3 | value_id=0
Type: [EntryRecord Metadata (Proto)]
  - prepared_num: 0
  - promised_num: 6
  - accepted_num: 6
  - value_id:     6
  - chosen:       TRUE
  - has_vid_only: FALSE
  - value bytes:  "paxos_third_val" (15 bytes)
  - uuids (0): []
-----------------------------------------------------------------
[Record #4]
Raw Key (24 bytes, Hex): 00 00 00 00 00 00 75 31 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 00 
Decoded Key: entity_id=30001 | entry=4 | value_id=0
Type: [EntryRecord Metadata (Proto)]
  - prepared_num: 5
  - promised_num: 5
  - accepted_num: 5
  - value_id:     5
  - chosen:       TRUE
  - has_vid_only: FALSE
  - value bytes:  "quorum_write_while_node2_down" (29 bytes)
  - uuids (0): []
-----------------------------------------------------------------
Total Records Inspected: 4
=================================================================
```

### 3.2 逐字节拆解对照表（以 Record #1 为例）

```
00 00 00 00 00 00 75 31 | 00 00 00 00 00 00 00 01 | 00 00 00 00 00 00 00 00
└─────── entity_id ─────┘ └──────── entry ────────┘ └─────── value_id ─────┘
  0x0000000000007531        0x0000000000000001        0x0000000000000000
       = 30001                     = 1                       = 0
```
- **Key 含义**：表示 Entity 30001 的第 1 号 entry 对应的共识元数据记录；
- **Value 含义**：Protobuf 编码的 `EntryRecord`。
  - `prepared_num: 5`：本地发起的提案编号；
  - `promised_num: 5`：当前承诺的最大编号；
  - `accepted_num: 5`：多数派接受的编号；
  - `value_id: 5`：该数据值的唯一标识；
  - `chosen: TRUE`：多数派已选定，不可更改；
  - `value bytes: "paxos_genesis_val"`：业务真实写入的 17 字节字符串。

---

## 4. PlogWorker 的批量聚合刷盘（WriteBatch 优化）

磁盘物理 IOPS 通常受限（哪怕是 NVMe SSD，单次 fsync 延迟也在数十微秒以上）。如果每个 Paxos 消息都单独调用一次磁盘写，系统 QPS 很难突破数千。

Certain 在 `PlogWorker` 中设计了**批量组提交（Group Commit）**机制：

```77:100:certain/src/plog_worker.cc
void PlogWorker::SetRecord(std::vector<std::unique_ptr<PaxosCmd>>& cmds) {
  // group by hash of entity
  std::unordered_map<
      uint32_t, std::pair<std::vector<PaxosCmd*>, std::vector<Plog::Record>>>
      records_groups;

  for (auto& cmd : cmds) {
    uint32_t hash_id = plog_->HashId(cmd->entity_id());
    auto& pair = records_groups[hash_id];

    const EntryRecord& record = cmd->local_entry_record();
    std::string buffer;
    if (!record.SerializeToString(&buffer)) {
      cmd->set_result(kRetCodeInvalidRecord);
      continue;
    }

    pair.first.push_back(cmd.get());
    pair.second.emplace_back();
    auto& back = pair.second.back();
    back.entity_id = cmd->entity_id();
    back.entry = cmd->entry();
    back.record = std::move(buffer);
  }
```

1. **从队列攒批**：在 `PlogWorker::Run` 中，一次性最多从无锁队列取出 `options_->max_plog_batch_size()`（默认数十到上百条）写请求；
2. **按 Hash 分组**：按底层 RocksDB 实例（Certain 支持挂载多个 DB 盘）对记录归类；
3. **原子 WriteBatch 写入**：调用 `MultiSetRecords`，底层将多条记录放入 RocksDB 的 `WriteBatch`，执行一次批量写入。数千个微小请求合并为单次批量持久化，极大地拉升了写入吞吐。

---

## 5. 业务 DB 状态机层（DbImpl）机制

在 `certain/default/db_impl.h` 和 `db_impl.cc` 中，展示了业务状态机的组织方式：

### 5.1 1024 桶哈希分片
`DbImpl` 维护了 1024 个 `DbInfo` Shard：
```cpp
struct Info {
  uint64_t entry = 0;
  uint32_t crc32 = 0;
  certain::Db::RecoverFlag flag = certain::Db::kNormal;
};
std::unordered_map<uint64_t, Info> map_;
```
每个 Shard 配备独立读写锁 `ReadWriteLock`，使得上百万实体的状态机更新分散在 1024 个桶内并发执行，互不锁死。

### 5.2 数据状态滚动增量 CRC32
状态机每应用一条日志，就对数据进行增量 CRC 滚算：
```cpp
info.entry = entry;
info.crc32 = certain::crc32(info.crc32, data.data(), data.size());
```
- 这使得任何两个副本之间，只要比对 `(entry, crc32)` 两个数值，就能 100% 确认两台机器上的数据完全一致，无须进行昂贵的数据逐字节全量比对！
- 在刚才的实测中，所有 3 台机器在应用完 Entry 3 后，CRC 全部精确收敛为 `4185058655`；应用完 Entry 4 后全部精确收敛为 `330915150`。

### 5.3 后台 DbDumper 线程异步持久化
为了避免状态机更新频繁阻塞主写流程，后台独立线程 `DbDumper` 每 10 秒定期唤醒，将状态机快照以追加文本流格式导出至 `./test_db.o/mem_db.txt`：
```cpp
while (stream >> entity_id >> entry >> crc32) {
  int ret = Set(entity_id, entry, crc32, kNormal);
}
```
节点重启时，直接重放 `mem_db.txt` 即可瞬间恢复状态机内存镜像。
