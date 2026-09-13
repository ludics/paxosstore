# 06. 高性能网络通信与 TinyRPC 架构深度解析

在高性能分布式存储系统中，网络通信层的性能和延迟往往决定了整个系统的上限。Certain 在网络通信层面采用了**双轨制设计**：
1. **Server 之间的 Paxos 协议内部通信**：基于 Linux `epoll`、专有二进制协议头 `MsgHeader`、长连接池与零拷贝写链表（`WriteItemList`）自研构建，追求极致吞吐与微秒级响应；
2. **Client 与 Server / 管理工具之间的 RPC 通信**：基于 `certain/tiny_rpc/` 实现，采用 Google Protobuf 原生 Service 接口、全双工协程通知模型，提供清晰易扩展的业务接口。

---

## 1. Server 间 Paxos 通信架构（`network/` 模块）

Certain 副本之间的网络通信位于 `certain/network/` 及 `certain/src/` 中，核心组件包括：
- `TcpSocket`：非阻塞 TCP 套接字封装；
- `Poller`：Linux `epoll_wait` 反应堆模型；
- `MsgChannel`：代表一条与对端 Server 的全双工网络信道；
- `MsgChannelHelper`：管理 Channel 的可读、可写事件注册与刷新；
- `WriteItemList`：高性能分散写（Scatter-Gather IO / `writev`）链表；
- `ConnWorker` 与 `ConnMng`：监听并维护节点间长连接池；
- `MsgWorker`：网络 I/O 线程（默认 48 个线程并行工作）。

### 1.1 跨节点长连接建立与分发（`ConnWorker` 与 `ConnMng`）

在 Paxos 集群中，副本间的连接是**预先建立、永久保活**的对等连接，避免在每次共识提案时经历 TCP 三次握手。

```
[集群其他节点] ──TCP SYN──> [Node 本地 Paxos 端口 (10066+idx)]
                                       │
                                       ▼
                                 [ConnWorker]
                                       │ Accept() 获得 TcpSocket
                                       ▼
                                  [ConnMng]
                    ┌──────────────────┴──────────────────┐
                    ▼                                     ▼
      TakeByMultiThread(Worker 0)           TakeByMultiThread(Worker 1)
                    │                                     │
                    ▼                                     ▼
             [MsgWorker 0]                         [MsgWorker 1]
     (注册到本地 epoll: MsgChannel)         (注册到本地 epoll: MsgChannel)
```

1. **`ConnWorker` 单独监听**：
   在 `certain/src/conn_worker.cc` 中，`ConnWorker` 独立线程仅监听 Paxos 互联端口，通过 `Accept` 接收来自其他副本的对等连接；
2. **`ConnMng` 无锁分发**：
   收到新的 Socket 后，通过 `ConnMng::GetInstance()->PutByMultiThread(worker_id, socket)` 将连接均衡哈希分配给具体的 `MsgWorker`；
3. **`MsgWorker::ServeNewConnect` 激活**：
   `MsgWorker` 在事件循环中取出属于自己的 Socket，包装为 `MsgChannel` 并注册进本地的 `Poller`（`epoll`），正式接管后续所有的读写事件。

---

### 1.2 高性能零拷贝写机制：`WriteItemList`

网络发送中最耗性能的是大报文的内存拷贝与频繁的 `write` 系统调用。Certain 自研了专用的分片写缓冲链表 `WriteItemList`（位于 `certain/network/write_item_list.h` 和 `write_item_list.cc`）。

```cpp
struct WriteItem {
  uint32_t len = 0;
  char* buffer = nullptr;
  WriteItem* next = nullptr;
};
```

#### 工作机制：
1. **追加缓冲（`Append`）**：
   当 `EntityWorker` 需要广播 Paxos 报文时，数据被序列化并直接放入 `WriteItem` 节点追加到链表尾部，无需预先连续大内存分配；
2. **聚合批量刷盘（`FlushBuffer` 与 `writev`）**：
   在 `MsgChannel::FlushBuffer` 中，如果链表中有多个等待发送的分片，系统将多个 `WriteItem` 映射为 `struct iovec` 数组，一次性调用 Linux 原生 **`writev`** 集中写入网络 Socket，显著降低系统调用次数；
3. **共享流控限制器**：
   为了防止网络拥塞导致发送端内存无限膨胀，`MsgChannel` 挂载了 `rbuf_shared_limiter_` 和 `wbuf_shared_limiter_`，一旦未发送数据超过上限，立即触发写背压（Back-pressure）。

---

## 2. Server 内部与外部消息传递全景机制

系统内部存在两级消息传递：
1. **进程内、跨线程组件间**：通过无锁队列传递 C++ 对象；
2. **跨机器、进程间**：通过网络传输专有二进制协议帧。

### 2.1 进程内线程间通信：`LockFreeQueue` 与所有权移动

在 Certain 内部，`MsgWorker`、`EntityWorker`、`PlogWorker`、`DbWorker` 完全运行在各自的独立线程中。
组件间的通信完全依托 **`AsyncQueueMng`** 统一调度的 **`LockFreeQueue<CmdBase>`**（无锁环形队列）。

#### 移动语义零拷贝（Zero-Copy Move Semantics）
```cpp
// 投递时：使用 std::unique_ptr 转移所有权，零堆拷贝
std::unique_ptr<PaxosCmd> pcmd = ...;
int ret = plog_req_queue_->PushByMultiThread(&pcmd);

// 消费时：单线程弹出，无互斥锁争用
std::unique_ptr<PaxosCmd> cmd;
int ret = plog_req_queue_->PopByOneThread(&cmd);
```
- **多生产者单消费者（MPSC）**：对于任意一个工作线程，其输入队列只有它自己在调用 `PopByOneThread`，其余工作线程调用 `PushByMultiThread`。无锁队列基于原子 CAS 操作推进指针，彻底避免了 `pthread_mutex` 引起的上下文切换与内核态陷阱。

---

### 2.2 进程间网络封包格式：`MsgHeader`

在不同 Server 进程之间，TCP 流必须被安全地解包与路由。Certain 定义了紧凑的 **8 字节协议头 `MsgHeader`**（位于 `certain/network/msg_header.h`）：

```cpp
struct __attribute__((packed)) MsgHeader {
  uint8_t magic_num;     // 1 字节：魔数 (固定为 0xfe)
  uint8_t msg_id;        // 1 字节：消息类型 (kCmdPaxos, kCmdRangeCatchup 等)
  uint16_t header_len;   // 2 字节：头部长度
  uint32_t body_len;     // 4 字节：后续 Body 载荷长度 (大端序)
};
```

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   magic_num   |    msg_id     |          header_len           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           body_len                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Protobuf Payload (PaxosMsg)                 |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### 解包与分发流程：
1. `MsgWorker::HandleRead` 触发，通过 `MsgChannel::ReadMore` 从 Socket 读取字节流；
2. 校验 `magic_num == 0xfe`，提取 `body_len`；
3. 读取完整的 Protobuf Body 后，调用 `MsgWorker::OnMessage`；
4. `CmdFactory::CreateCmd` 将二进制反序列化为 `PaxosCmd`，根据 `Hash(entity_id)` 塞入目标 `EntityWorker` 的 `entity_req_queue_`，完成了从物理网络到内存共识流水线的无缝切换！

---

## 3. TinyRPC 深度实现与主要用途

除了副本间的内部通信外，Certain 还内置了一个轻量级的 RPC 框架 —— **`tiny_rpc`**（位于 `certain/tiny_rpc/`）。

### 3.1 为什么自研 TinyRPC？
1. **消除重型第三方依赖**：生产环境避免引入 gRPC 等庞大、容易产生版本冲突与复杂线程调度的框架；
2. **深度契合 libco 协程**：天然与 Tencent `libco` 结合，支持全双工、高并发的协程级调度；
3. **原生兼容 Protobuf RPC 标准**：直接继承 Google 原生 `google::protobuf::Service`、`RpcChannel` 和 `RpcController` 抽象接口。

### 3.2 架构实现剖析

```
                 [ Client 端 ]                            [ Server 端 ]
              TinyClient (Stub)                         TinyServer
                      │                                       │
                      ▼                                       ▼
                 TinyChannel                       RoutineWorker<TcpSocket>
           (包装 MsgHeader 发送请求)                    (64 协程并发监听与解析)
                      │                                       │
                      │────────────── TCP 链路 ───────────────>│
                      │                                       ▼
                      │                                 DoJob 路由分发
                      │                                 (调用 TinyServiceImpl)
                      │                                       │
                      │<───────────── 结果回包 ────────────────│
                      ▼
         TinyRpc::ReceiveResponse
         (唤醒业务协程，提取 RetCode)
```

#### 关键源码实现：
1. **服务端基于 64 协程并发轮询（`RoutineWorker<TcpSocket>`）**：
   在 `certain/tiny_rpc/tiny_server.h` 中：
   ```cpp
   class TinyServer : public RoutineWorker<TcpSocket> {
    public:
     TinyServer(std::string local_addr, google::protobuf::Service* service)
         : RoutineWorker<TcpSocket>("tiny_server", 64), ...
   ```
   单端口默认启动 64 个轻量级协程并发执行 `co_eventloop`，每个接入的客户端连接由一个空闲协程挂载，协程内部同步编写非阻塞的 `ReceiveHeader` $\to$ `ReceiveBody` $\to$ `CallMethod` $\to$ `SendMessage`，兼顾了同步编写的简洁性与异步高并发的高性能。

2. **请求路由与反射调用**：
   在 `certain/tiny_rpc/tiny_server.cc` 的 `DoJob` 中：
   ```cpp
   int type = header.msg_id;
   auto descriptor = service_->GetDescriptor();
   auto method = descriptor->method(type);

   std::unique_ptr<::google::protobuf::Message> req(
       service_->GetRequestPrototype(method).New());
   std::unique_ptr<::google::protobuf::Message> rsp(
       service_->GetResponsePrototype(method).New());
   TinyRpc::ReceiveBody(socket.get(), req.get(), &header);

   TinyController controller;
   service_->CallMethod(method, &controller, req.get(), rsp.get(), nullptr);
   TinyRpc::SendMessage(socket.get(), *rsp, type, controller.RetCode());
   ```
   无需为每个接口手写路由表，直接利用 Protobuf 的 `MethodDescriptor` 和方法下标索引完成动态调度！

### 3.3 TinyRPC 的主要用途
TinyRPC 在 Certain 中承担了三个核心职责：
1. **客户端业务服务（`TinyServiceImpl`，默认端口 11066+idx）**：
   对外暴露 `Write`、`Read`、`AppendString`、`GetStringStatus` 接口，接收应用方的数据读写指令；
2. **运维与管控服务（`ToolsServiceImpl`，默认端口 12066+idx）**：
   暴露 `DumpEntry`、`DumpEntity` 等指令，供命令行工具实时提取特定 Entity 在内存中的 Paxos 状态机镜像；
3. **快照数据传输（`SnapshotRecover`）**：
   当节点严重落后且 Plog 已经截断时，通过 TinyRPC 的 `SnapshotRecover` 接口从健康节点全量拉取业务 DB 快照。

---

## 4. TinyRPC 纯性能实测与压测分析

为了评估 TinyRPC 的性能与开销，我们编写了独立的基准测试程序 `certain/tools/tinyrpc_bench.cc`，在完全排除磁盘 I/O 和 Paxos 投票干扰的前提下，测试 TinyRPC 本身的吞吐上限与网络开销。

### 4.1 压测数据与执行结果
```bash
./certain/build/bin/tinyrpc_bench --threads=4 --requests=5000
```

```
Testing TinyRPC loopback on 127.0.0.1:19999 with 4 threads x 5000 reqs...
TinyRPC Pure RPC Performance:
  Total Requests: 20000
  Elapsed Time:   0.648 s
  Throughput QPS: 30881.475 req/s
  Avg Latency:    0.130 ms (130 μs)
```

### 4.2 性能结论
- **吞吐量**：在 4 线程并发下，单进程 TinyRPC 轻松达成 **30,881 QPS**；
- **纯网络时延**：平均往返延迟仅 **130 微秒（0.130 ms）**；
- **结论**：TinyRPC 自身的协议打包、解析与协程调度开销极低（仅占毫秒级总耗时的不到 2%），完全不会成为共识存储流水线的性能瓶颈。
