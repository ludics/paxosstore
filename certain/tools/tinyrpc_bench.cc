#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "default/log_impl.h"
#include "gflags/gflags.h"
#include "proto/tiny_rpc.pb.h"
#include "tiny_rpc/tiny_client.h"
#include "tiny_rpc/tiny_server.h"

class EchoServiceImpl : public certain::TinyRpcService {
 public:
  void Write(google::protobuf::RpcController* controller,
             const certain::WriteReq* request, certain::WriteRsp* response,
             google::protobuf::Closure* done) override {
    auto c = static_cast<certain::TinyController*>(controller);
    c->SetRetCode(0);
  }
};

DEFINE_int32(port, 19999, "Benchmark port");
DEFINE_int32(threads, 4, "Concurrent threads");
DEFINE_int32(requests, 5000, "Requests per thread");

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  LogImpl log("mm", "./test_log.o", certain::LogLevel::kFatal, 1);
  if (log.Init() == 0) {
    certain::Log::GetInstance()->Init(&log);
  }

  std::string addr = "127.0.0.1:" + std::to_string(FLAGS_port);
  EchoServiceImpl service;
  certain::TinyServer server(addr, &service);
  int ret = server.Init();
  if (ret != 0) {
    std::cerr << "Server init failed: " << ret << std::endl;
    return 1;
  }
  server.Start();

  // Allow listen ready
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::cout << "Testing TinyRPC loopback on " << addr << " with "
            << FLAGS_threads << " threads x " << FLAGS_requests << " reqs...\n";

  auto t0 = std::chrono::high_resolution_clock::now();
  std::vector<std::thread> workers;
  std::atomic<uint64_t> total_succ{0};

  for (int t = 0; t < FLAGS_threads; ++t) {
    workers.emplace_back([&]() {
      certain::TinyClient client(addr);
      for (int i = 0; i < FLAGS_requests; ++i) {
        certain::TinyController controller;
        certain::WriteReq req;
        req.set_entity_id(12345);
        req.set_entry(i + 1);
        req.set_value("echo_test_payload");
        certain::WriteRsp rsp;
        client.Write(&controller, &req, &rsp, nullptr);
        if (controller.RetCode() == 0) {
          total_succ++;
        }
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }
  auto t1 = std::chrono::high_resolution_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();

  std::cout << "TinyRPC Pure RPC Performance:\n"
            << "  Total Requests: " << total_succ.load() << "\n"
            << "  Elapsed Time:   " << std::fixed << std::setprecision(3) << sec << " s\n"
            << "  Throughput QPS: " << (total_succ.load() / sec) << " req/s\n"
            << "  Avg Latency:    " << (sec * 1000.0 * FLAGS_threads / total_succ.load()) << " ms\n";

  server.set_exit_flag(true);
  server.WaitExit();
  return 0;
}
