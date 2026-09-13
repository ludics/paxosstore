# 10. Certain 源码深度研读路线与实战调试实验指南

研读分布式共识系统（如 Multi-Paxos）的源码往往具有较高的认知门槛：多线程并发、异步队列流转、复杂的网络事件和瞬息万变的状态机迁移很容易让人迷失。

本篇文档为有志于彻底吃透 Certain 工业级实现的研究者与工程师，提供一套经过实战检验的**系统化学习路线**、**源码追踪地图**以及**高阶实验调试技巧**。

---

## 1. 科学的源码研读路线图（四阶段进阶）

不要一上来就扎进成千上万行的网络或 Worker 线程逻辑中，建议按照以下**自底向上、由静态到动态**的四个阶段推进：

```
┌──────────────────────────────────────────────────────────┐
│  阶段一：静态数据结构与理论映射 (最纯粹的算法数学模型)      │
│  - proto/certain.proto, src/entry_state.h / .cc          │
│  - 目标：吃透 EntryStateMachine 7 大状态与无碰撞编号公式    │
└────────────────────────────┬─────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────┐
│  阶段二：无锁管线与组件解耦 (流水线骨架)                  │
│  - utils/lock_free_queue.h, src/async_queue_mng.h        │
│  - src/entity_worker.cc, src/plog_worker.cc              │
│  - 目标：理清数据在无锁队列中是如何跨线程流动的            │
└────────────────────────────┬─────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────┐
│  阶段三：端到端状态机决策中枢 (核心业务共识链路)          │
│  - src/entity_helper.h / .cc                             │
│  - 目标：精读 HandleClientCmd, HandleWriteCmd,           │
│         UpdateMachineByPaxosCmd, HandleSetFromPlog       │
└────────────────────────────┬─────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────┐
│  阶段四：网络、I/O 存储与故障自愈 (工业级健壮性防线)        │
│  - network/*, tiny_rpc/*, default/plog_impl.cc           │
│  - src/catchup_worker.cc, src/recover_worker.cc          │
│  - 目标：掌握零拷贝网络收发、RocksDB 组提交与快照恢复机制  │
└──────────────────────────────────────────────────────────┘
```

---

## 2. 关键代码锚点与函数必读清单

| 关注维度 | 核心文件 | 必读核心函数 / 类 | 研读要点与思考题 |
| :--- | :--- | :--- | :--- |
| **状态机** | `certain/src/entry_state.cc` | `EntryStateMachine::Promise`<br>`EntryStateMachine::Accept`<br>`EntryStateMachine::CalcEntryState` | 为什么 $pn$ 生成公式中有一句 `if (!pre_auth && pn <= n) pn += n`？`Accept` 中如何实现 Lamport P2c 不变式？ |
| **中枢调度** | `certain/src/entity_helper.cc` | `EntityHelper::HandleWriteCmd`<br>`EntityHelper::UpdateMachineByPaxosCmd`<br>`EntityHelper::HandleSetFromPlog` | 为什么写网络之前必须先落盘 PLog？收到多数派承诺时如何自动触发 Accept？ |
| **流水线** | `certain/src/entity_worker.cc` | `EntityWorker::HandleEvents` | 为什么先处理 `user_req`，再处理 `entity_req` 和 `plog_rsp`？事件循环顺序对延迟有何影响？ |
| **磁盘存储** | `certain/default/plog_impl.cc` | `EntryKey`<br>`PlogImpl::LoadMaxEntry`<br>`PlogImpl::MultiSetRecords` | 24 字节大端序 Key 如何利用 RocksDB 字典序实现 $O(\log N)$ 寻址？ |
| **网络引擎** | `certain/network/msg_channel.cc` | `MsgChannel::ReadMore`<br>`MsgChannel::FlushBuffer` | `WriteItemList` 是如何利用 `writev` 批量写避免频繁系统调用的？ |
| **故障追赶** | `certain/src/catchup_worker.cc` | `CatchupWorker::DoJob` | `TrafficLimiter` 是如何使用双重令牌桶对流量和 QPS 实施限流保护的？ |

---

## 3. 实战实验与动态调试技巧

静态看代码往往容易产生“似懂非懂”的错觉，只有通过真实运行、抓取日志、打印状态机和注入故障，才能真正建立物理直觉。

### 3.1 技巧一：开启 Debug 级日志观察 Paxos 报文收发
默认情况下，Server 打印的日志等级较高。通过指定 `--log_level=5`（Debug 级别），可以实时捕获状态机的每一次跳跃：

```bash
# 在终端中启动节点并重定向到单独日志文件
./build/bin/server --index=0 --data_dir=build/run/node0 --log_level=5 > node0_debug.log 2>&1 &
```
在日志中搜索以下高价值关键词：
- `grep "Promise" node0_debug.log`：观察本地编号生成与承诺；
- `grep "kMajorityPromise" node0_debug.log`：观察多数派承诺达成的瞬间；
- `grep "kChosen" node0_debug.log`：观察决议达成的精准时刻；
- `grep "SwitchToLocalView" node0_debug.log`：观察半对称报文在网络到达后的视角对调。

---

### 3.2 技巧二：使用 `dump_entry` 在线捕获内存状态机镜像
Certain 专门提供了运维探测工具 `dump_entry`，它通过 TinyRPC 连接正在运行中的节点（默认 Tools 端口为 12066+idx），实时输出目标槽位的状态快照：

```bash
# 探测 Entity 30001 的 Entry 1 在 Node 0 上的运行态状态机
./build/bin/dump_entry --server_ip=127.0.0.1 --server_port=12066 --entity_id=30001 --entry=1
```
输出示例解读：
```
Entity Info: e 30001 an 3 local 0 entrys: 1 1 1 0 auth 1 active 1 ...
Entry Info: E(30001, 1) uncertain 0 broad 0 sync -1 comp 0 ... st 6
Machine State: st 6 local 0 r0:[5 5 5 vid 5 u.sz 0 v.sz 17 cho 1] r1:[0 5 5 vid 5 u.sz 0 v.sz 17 cho 0] r2:[0 5 5 vid 5 u.sz 0 v.sz 17 cho 0]
```
- `st 6`：对应 `EntryState::kChosen`；
- `r0:[5 5 5 ... cho 1]`：Node 0 本地视角，prepared=5, promised=5, accepted=5, chosen=1；
- `r1/r2`：Node 0 记录的对端状态，对端也均已 accepted=5。多数派达成，一览无余！

---

### 3.3 技巧三：使用 `inspect_plog` 穿透文件系统直视二进制
代码跑完后，数据在硬盘上到底长什么样？运行我们开发的 `inspect_plog` 工具：

```bash
./build/bin/inspect_plog --db_path=build/experiment_artifacts/node0/test_plog.o
```
你可以亲眼看到：
1. 24 字节十六进制大端序 Key 的每一位；
2. Protobuf 序列化后的 `EntryRecord` 字段值；
3. 真实业务写入的 Payload 字符串。

---

### 3.4 技巧四：故障注入实验设计（Chaos Testing）

在分布式系统中，以下几个故障实验非常值得动手尝试：

#### 实验 A：单节点拔线与恢复（网络分区模拟）
```bash
# 启动 3 节点集群后，使用 iptables 屏蔽 Node 2 的网络互联端口 (10068)
sudo iptables -A INPUT -p tcp --dport 10068 -j DROP
sudo iptables -A OUTPUT -p tcp --dport 10068 -j DROP

# 此时向 Node 0 写入，观察是否依然成功（因为 0 和 1 构成 2/3 多数派）
./build/bin/client --cmd=write --server_port=11066 --entity_id=40001 --entry=1 --value="paxos_under_partition"

# 解除网络屏蔽
sudo iptables -D INPUT -p tcp --dport 10068 -j DROP
sudo iptables -D OUTPUT -p tcp --dport 10068 -j DROP

# 在 Node 2 上读取，观察日志中 CatchupWorker 如何自动拉回数据自愈
./build/bin/client --cmd=getstringstatus --server_port=11068 --entity_id=40001
```

#### 实验 B：单节点强制杀死与冷备快照恢复
1. 正常写入 10 条日志；
2. 强杀 Node 2（`kill -9`）；
3. 在存活节点上继续写入数千条日志并触发 PLog GC；
4. 重启 Node 2，观察 Node 2 的日志，验证其在收到 `NotFound` 后如何自动优雅触发 `SnapshotRecover`，将整个 DB 快照瞬间追平！

---

## 4. 总结与进阶展望

Certain 作为微信核心海量存储支撑框架，其代码风格凝练、工程极简度极高（C++11 现代实现、零额外无用抽象、流水线性能压榨到极致）。

通过本套系统文档的深入研读与配套实验验证，你不仅能够彻底掌握 Leslie Lamport 的经典 Paxos 在工业界是如何落地的，更能深刻体会到在超高吞吐、微秒级延迟要求下，**无锁队列、内存布局大端序对齐、半对称报文合并与状态机双游标解耦**等卓越架构设计的巨大魅力！
