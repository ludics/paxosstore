#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "default/log_impl.h"
#include "gflags/gflags.h"
#include "proto/tiny_rpc.pb.h"
#include "tiny_rpc/tiny_client.h"

DEFINE_string(server_ip, "127.0.0.1", "Cluster node IP");
DEFINE_int32(server_port, 11066, "Cluster node RPC port");
DEFINE_int32(threads, 4, "Number of concurrent benchmark threads");
DEFINE_int32(requests_per_thread, 250, "Requests per thread");
DEFINE_int32(value_size, 64, "Size of payload bytes");
DEFINE_string(cmd, "appendstring", "Command type: write, appendstring, read");
DEFINE_uint64(base_entity, 50000, "Base entity ID to avoid collision");

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  LogImpl log("mm", "./test_log.o", certain::LogLevel::kError, 1);
  if (log.Init() == 0) {
    certain::Log::GetInstance()->Init(&log);
  }

  std::string server_addr = FLAGS_server_ip + ":" + std::to_string(FLAGS_server_port);
  std::string payload(FLAGS_value_size, 'x');

  std::cout << "=================================================================\n";
  std::cout << "  Starting Certain Paxos Cluster Benchmark\n";
  std::cout << "  Target Node: " << server_addr << "\n";
  std::cout << "  Command:     " << FLAGS_cmd << "\n";
  std::cout << "  Threads:     " << FLAGS_threads << "\n";
  std::cout << "  Req/Thread:  " << FLAGS_requests_per_thread << "\n";
  std::cout << "  Total Req:   " << (FLAGS_threads * FLAGS_requests_per_thread) << "\n";
  std::cout << "  Payload:     " << FLAGS_value_size << " bytes\n";
  std::cout << "=================================================================\n";

  std::atomic<uint64_t> success_count{0};
  std::atomic<uint64_t> fail_count{0};
  std::mutex lat_mutex;
  std::vector<double> latencies_ms;
  latencies_ms.reserve(FLAGS_threads * FLAGS_requests_per_thread);

  auto start_time = std::chrono::high_resolution_clock::now();

  std::vector<std::thread> workers;
  for (int t = 0; t < FLAGS_threads; ++t) {
    workers.emplace_back([&, t]() {
      certain::TinyClient client(server_addr);
      uint64_t entity_id = FLAGS_base_entity + t;
      std::vector<double> local_lats;
      local_lats.reserve(FLAGS_requests_per_thread);

      for (int r = 0; r < FLAGS_requests_per_thread; ++r) {
        auto t0 = std::chrono::high_resolution_clock::now();
        certain::TinyController controller;

        if (FLAGS_cmd == "appendstring") {
          certain::AppendStringReq req;
          req.set_entity_id(entity_id);
          req.set_value(payload);
          certain::AppendStringRsp rsp;
          client.AppendString(&controller, &req, &rsp, nullptr);
        } else if (FLAGS_cmd == "write") {
          certain::WriteReq req;
          req.set_entity_id(entity_id);
          req.set_entry(r + 1);
          req.set_value(payload);
          certain::WriteRsp rsp;
          client.Write(&controller, &req, &rsp, nullptr);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        local_lats.push_back(ms);

        if (controller.RetCode() == 0) {
          success_count++;
        } else {
          fail_count++;
        }
      }

      std::lock_guard<std::mutex> lock(lat_mutex);
      latencies_ms.insert(latencies_ms.end(), local_lats.begin(), local_lats.end());
    });
  }

  for (auto& th : workers) {
    th.join();
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  double total_duration_sec = std::chrono::duration<double>(end_time - start_time).count();

  std::sort(latencies_ms.begin(), latencies_ms.end());
  double avg_lat = 0;
  for (double d : latencies_ms) avg_lat += d;
  if (!latencies_ms.empty()) avg_lat /= latencies_ms.size();

  double p50 = latencies_ms.empty() ? 0 : latencies_ms[latencies_ms.size() * 0.50];
  double p90 = latencies_ms.empty() ? 0 : latencies_ms[latencies_ms.size() * 0.90];
  double p99 = latencies_ms.empty() ? 0 : latencies_ms[latencies_ms.size() * 0.99];
  double qps = total_duration_sec > 0 ? (success_count.load() / total_duration_sec) : 0;

  std::cout << "\n---------------- Benchmark Results ----------------\n";
  std::cout << "Elapsed Time:       " << std::fixed << std::setprecision(2) << total_duration_sec << " s\n";
  std::cout << "Successful Req:     " << success_count.load() << "\n";
  std::cout << "Failed Req:         " << fail_count.load() << "\n";
  std::cout << "Success Rate:       " << std::fixed << std::setprecision(2)
            << (100.0 * success_count.load() / (success_count.load() + fail_count.load())) << " %\n";
  std::cout << "Throughput (QPS):   " << std::fixed << std::setprecision(2) << qps << " req/sec\n";
  std::cout << "Latency Avg:        " << std::fixed << std::setprecision(3) << avg_lat << " ms\n";
  std::cout << "Latency P50:        " << std::fixed << std::setprecision(3) << p50 << " ms\n";
  std::cout << "Latency P90:        " << std::fixed << std::setprecision(3) << p90 << " ms\n";
  std::cout << "Latency P99:        " << std::fixed << std::setprecision(3) << p99 << " ms\n";
  std::cout << "=================================================================\n";

  return 0;
}
