// Microbenchmark: Certain LIGHTLIST vs Linux list_head (userspace copy)
// vs std::list (non-intrusive). C++11. Intrusive nodes are preallocated.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <vector>

#include <sched.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

#include "utils/light_list.h"
#include "utils/light_list_modern.h"

namespace {

// Userspace subset of include/linux/list.h (circular dummy head).
// Unlink with list_del_init so membership is !list_empty(&node->link).
struct list_head {
  list_head* next;
  list_head* prev;
};

inline void INIT_LIST_HEAD(list_head* list) {
  list->next = list;
  list->prev = list;
}

inline void __list_add(list_head* neu, list_head* prev, list_head* next) {
  next->prev = neu;
  neu->next = next;
  neu->prev = prev;
  prev->next = neu;
}

inline void list_add(list_head* neu, list_head* head) {
  __list_add(neu, head, head->next);
}

inline void list_add_tail(list_head* neu, list_head* head) {
  __list_add(neu, head->prev, head);
}

inline void __list_del(list_head* prev, list_head* next) {
  next->prev = prev;
  prev->next = next;
}

inline void list_del_init(list_head* entry) {
  __list_del(entry->prev, entry->next);
  INIT_LIST_HEAD(entry);
}

inline bool list_empty(const list_head* head) { return head->next == head; }

#define LIST_ENTRY(ptr, type, member) \
  reinterpret_cast<type*>(reinterpret_cast<char*>(ptr) - offsetof(type, member))

template <typename T>
inline void DoNotOptimize(T const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

inline uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool PinToCpu0() {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
}

struct LightNode {
  uint64_t id;
  LIGHTLIST_ENTRY(LightNode) link;
};

typedef LIGHTLIST(LightNode) LightList;

struct ModernNode {
  uint64_t id;
  certain::LightListHook<ModernNode> link;
};

typedef certain::LightList<ModernNode, &ModernNode::link> ModernList;

struct KernelNode {
  uint64_t id;
  list_head link;
};

struct StdNode {
  uint64_t id;
};

struct Stats {
  uint64_t ns_min;
  uint64_t ns_med;
  uint64_t ns_max;
  uint64_t checksum;
};

Stats Summarize(std::vector<uint64_t>* samples, uint64_t checksum) {
  std::sort(samples->begin(), samples->end());
  Stats s;
  s.ns_min = samples->front();
  s.ns_max = samples->back();
  s.ns_med = (*samples)[samples->size() / 2];
  s.checksum = checksum;
  return s;
}

void PrintRow(const char* impl, const char* workload, uint64_t ops,
              const Stats& s) {
  const double ns_op = static_cast<double>(s.ns_med) / static_cast<double>(ops);
  std::printf("%-12s %-18s %10.2f %10.2f %10.2f  %16llu\n", impl, workload,
              static_cast<double>(s.ns_min) / static_cast<double>(ops), ns_op,
              static_cast<double>(s.ns_max) / static_cast<double>(ops),
              static_cast<unsigned long long>(s.checksum));
}

void LightInitNodes(LightNode* nodes, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    nodes[i].id = i;
    LIGHTLIST_ENTRY_INIT(&nodes[i], link);
  }
}

void ModernInitNodes(ModernNode* nodes, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    nodes[i].id = i;
    nodes[i].link.reset();
  }
}

uint64_t ModernInsertHead(ModernList* list, ModernNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list->insert_head(&nodes[i]);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t ModernInsertTail(ModernList* list, ModernNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list->insert_tail(&nodes[i]);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t ModernDrainHead(ModernList* list) {
  uint64_t sink = 0;
  while (!list->empty()) {
    ModernNode* node = list->first();
    sink += node->id;
    list->remove(node);
  }
  return sink;
}

uint64_t ModernDrainTail(ModernList* list) {
  uint64_t sink = 0;
  while (!list->empty()) {
    ModernNode* node = list->last();
    sink += node->id;
    list->remove(node);
  }
  return sink;
}

uint64_t ModernIterate(ModernList* list) {
  uint64_t sink = 0;
  for (ModernNode* node = list->first(); !list->is_sentinel(node);
       node = list->next(node)) {
    sink += node->id;
  }
  return sink;
}

uint64_t ModernMembership(ModernNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    sink += ModernList::contains(&nodes[i]) ? 1 : 0;
  }
  return sink;
}

uint64_t ModernLruTouch(ModernList* list, ModernNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list->remove(&nodes[i]);
    list->insert_head(&nodes[i]);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t ModernRemoveRand(ModernList* list, ModernNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (i * 1103515245u + 12345u) % n;
    if (ModernList::contains(&nodes[idx])) {
      list->remove(&nodes[idx]);
      sink += nodes[idx].id;
    }
  }
  return sink;
}

void KernelInitNodes(KernelNode* nodes, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    nodes[i].id = i;
    INIT_LIST_HEAD(&nodes[i].link);
  }
}

uint64_t LightInsertHead(LightList* list, LightNode* nodes,
                         size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    LIGHTLIST_INSERT_HEAD(list, &nodes[i], link);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t LightInsertTail(LightList* list, LightNode* nodes,
                         size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    LIGHTLIST_INSERT_TAIL(list, &nodes[i], link);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t LightDrainHead(LightList* list) {
  uint64_t sink = 0;
  while (!LIGHTLIST_EMPTY(list)) {
    LightNode* node = LIGHTLIST_FIRST(list);
    sink += node->id;
    LIGHTLIST_REMOVE(list, node, link);
  }
  return sink;
}

uint64_t LightDrainTail(LightList* list) {
  uint64_t sink = 0;
  while (!LIGHTLIST_EMPTY(list)) {
    LightNode* node = LIGHTLIST_LAST(list);
    sink += node->id;
    LIGHTLIST_REMOVE(list, node, link);
  }
  return sink;
}

uint64_t LightIterate(LightList* list) {
  uint64_t sink = 0;
  for (LightNode* node = LIGHTLIST_FIRST(list); (void*)node != (void*)list;
       node = node->link.next) {
    sink += node->id;
  }
  return sink;
}

uint64_t LightMembership(LightNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    sink += ENTRY_IN_LIGHTLIST(&nodes[i], link) ? 1 : 0;
  }
  return sink;
}

uint64_t LightLruTouch(LightList* list, LightNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    LIGHTLIST_REMOVE(list, &nodes[i], link);
    LIGHTLIST_INSERT_HEAD(list, &nodes[i], link);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t LightRemoveRand(LightList* list, LightNode* nodes,
                         size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (i * 1103515245u + 12345u) % n;
    if (ENTRY_IN_LIGHTLIST(&nodes[idx], link)) {
      LIGHTLIST_REMOVE(list, &nodes[idx], link);
      sink += nodes[idx].id;
    }
  }
  return sink;
}

uint64_t KernelInsertHead(list_head* head, KernelNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list_add(&nodes[i].link, head);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t KernelInsertTail(list_head* head, KernelNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list_add_tail(&nodes[i].link, head);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t KernelDrainHead(list_head* head) {
  uint64_t sink = 0;
  while (!list_empty(head)) {
    KernelNode* node = LIST_ENTRY(head->next, KernelNode, link);
    sink += node->id;
    list_del_init(&node->link);
  }
  return sink;
}

uint64_t KernelDrainTail(list_head* head) {
  uint64_t sink = 0;
  while (!list_empty(head)) {
    KernelNode* node = LIST_ENTRY(head->prev, KernelNode, link);
    sink += node->id;
    list_del_init(&node->link);
  }
  return sink;
}

uint64_t KernelIterate(list_head* head) {
  uint64_t sink = 0;
  for (list_head* p = head->next; p != head; p = p->next) {
    KernelNode* node = LIST_ENTRY(p, KernelNode, link);
    sink += node->id;
  }
  return sink;
}

uint64_t KernelMembership(KernelNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    sink += list_empty(&nodes[i].link) ? 0 : 1;
  }
  return sink;
}

uint64_t KernelLruTouch(list_head* head, KernelNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list_del_init(&nodes[i].link);
    list_add(&nodes[i].link, head);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t KernelRemoveRand(KernelNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (i * 1103515245u + 12345u) % n;
    if (!list_empty(&nodes[idx].link)) {
      list_del_init(&nodes[idx].link);
      sink += nodes[idx].id;
    }
  }
  return sink;
}

uint64_t StdInsertHead(std::list<StdNode*>* list, StdNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list->push_front(&nodes[i]);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t StdInsertTail(std::list<StdNode*>* list, StdNode* nodes, size_t n) {
  uint64_t sink = 0;
  for (size_t i = 0; i < n; ++i) {
    list->push_back(&nodes[i]);
    sink += nodes[i].id;
  }
  return sink;
}

uint64_t StdDrainHead(std::list<StdNode*>* list) {
  uint64_t sink = 0;
  while (!list->empty()) {
    sink += list->front()->id;
    list->pop_front();
  }
  return sink;
}

uint64_t StdIterate(const std::list<StdNode*>* list) {
  uint64_t sink = 0;
  for (std::list<StdNode*>::const_iterator it = list->begin();
       it != list->end(); ++it) {
    sink += (*it)->id;
  }
  return sink;
}

}  // namespace

int main(int argc, char** argv) {
  size_t n = 100000;
  int rounds = 80;
  if (argc > 1) n = static_cast<size_t>(std::strtoull(argv[1], NULL, 10));
  if (argc > 2) rounds = std::atoi(argv[2]);
  if (n < 16 || rounds < 5) {
    std::fprintf(stderr, "usage: %s [n>=16] [rounds>=5]\n", argv[0]);
    return 1;
  }

  const bool pinned = PinToCpu0();
  const int warmup = 3;
  const int total = rounds + warmup;
  const uint64_t expected_sum = static_cast<uint64_t>(n) * (n - 1) / 2;

  std::fprintf(stderr,
               "light_list_bench n=%zu rounds=%d cpu0_pinned=%s "
               "CLOCK_MONOTONIC RelWithDebInfo\n",
               n, rounds, pinned ? "yes" : "no");
  std::printf("%-12s %-18s %10s %10s %10s  %16s\n", "impl", "workload",
              "min_ns/op", "med_ns/op", "max_ns/op", "checksum");

  std::vector<LightNode> light_nodes(n);
  std::vector<ModernNode> modern_nodes(n);
  std::vector<KernelNode> kernel_nodes(n);
  std::vector<StdNode> std_nodes(n);
  for (size_t i = 0; i < n; ++i) {
    std_nodes[i].id = i;
  }

  LightList light_list;
  ModernList modern_list;
  list_head kernel_head;
  std::list<StdNode*> std_list;

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s, std_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0, std_c = 0;
    for (int r = 0; r < total; ++r) {
      LightInitNodes(light_nodes.data(), n);
      LIGHTLIST_INIT(&light_list);
      uint64_t t0 = NowNs();
      light_c = LightInsertHead(&light_list, light_nodes.data(), n);
      uint64_t dt = NowNs() - t0;
      DoNotOptimize(light_c);
      if (r >= warmup) light_s.push_back(dt);
      LightDrainHead(&light_list);

      ModernInitNodes(modern_nodes.data(), n);
      modern_list.clear();
      t0 = NowNs();
      modern_c = ModernInsertHead(&modern_list, modern_nodes.data(), n);
      dt = NowNs() - t0;
      DoNotOptimize(modern_c);
      if (r >= warmup) modern_s.push_back(dt);
      ModernDrainHead(&modern_list);

      KernelInitNodes(kernel_nodes.data(), n);
      INIT_LIST_HEAD(&kernel_head);
      t0 = NowNs();
      kernel_c = KernelInsertHead(&kernel_head, kernel_nodes.data(), n);
      dt = NowNs() - t0;
      DoNotOptimize(kernel_c);
      if (r >= warmup) kernel_s.push_back(dt);
      KernelDrainHead(&kernel_head);

      std_list.clear();
      t0 = NowNs();
      std_c = StdInsertHead(&std_list, std_nodes.data(), n);
      dt = NowNs() - t0;
      DoNotOptimize(std_c);
      if (r >= warmup) std_s.push_back(dt);
      std_list.clear();
    }
    PrintRow("light_list", "insert_head", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "insert_head", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "insert_head", n, Summarize(&kernel_s, kernel_c));
    PrintRow("std::list", "insert_head", n, Summarize(&std_s, std_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s, std_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0, std_c = 0;
    for (int r = 0; r < total; ++r) {
      LightInitNodes(light_nodes.data(), n);
      LIGHTLIST_INIT(&light_list);
      uint64_t t0 = NowNs();
      light_c = LightInsertTail(&light_list, light_nodes.data(), n);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);
      LightDrainHead(&light_list);

      ModernInitNodes(modern_nodes.data(), n);
      modern_list.clear();
      t0 = NowNs();
      modern_c = ModernInsertTail(&modern_list, modern_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);
      ModernDrainHead(&modern_list);

      KernelInitNodes(kernel_nodes.data(), n);
      INIT_LIST_HEAD(&kernel_head);
      t0 = NowNs();
      kernel_c = KernelInsertTail(&kernel_head, kernel_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);
      KernelDrainHead(&kernel_head);

      std_list.clear();
      t0 = NowNs();
      std_c = StdInsertTail(&std_list, std_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) std_s.push_back(dt);
      std_list.clear();
    }
    PrintRow("light_list", "insert_tail", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "insert_tail", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "insert_tail", n, Summarize(&kernel_s, kernel_c));
    PrintRow("std::list", "insert_tail", n, Summarize(&std_s, std_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s, std_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0, std_c = 0;
    for (int r = 0; r < total; ++r) {
      LightInitNodes(light_nodes.data(), n);
      LIGHTLIST_INIT(&light_list);
      LightInsertHead(&light_list, light_nodes.data(), n);
      uint64_t t0 = NowNs();
      light_c = LightDrainHead(&light_list);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);

      ModernInitNodes(modern_nodes.data(), n);
      modern_list.clear();
      ModernInsertHead(&modern_list, modern_nodes.data(), n);
      t0 = NowNs();
      modern_c = ModernDrainHead(&modern_list);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);

      KernelInitNodes(kernel_nodes.data(), n);
      INIT_LIST_HEAD(&kernel_head);
      KernelInsertHead(&kernel_head, kernel_nodes.data(), n);
      t0 = NowNs();
      kernel_c = KernelDrainHead(&kernel_head);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);

      std_list.clear();
      StdInsertHead(&std_list, std_nodes.data(), n);
      t0 = NowNs();
      std_c = StdDrainHead(&std_list);
      dt = NowNs() - t0;
      if (r >= warmup) std_s.push_back(dt);
    }
    PrintRow("light_list", "drain_head", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "drain_head", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "drain_head", n, Summarize(&kernel_s, kernel_c));
    PrintRow("std::list", "drain_head", n, Summarize(&std_s, std_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0;
    for (int r = 0; r < total; ++r) {
      LightInitNodes(light_nodes.data(), n);
      LIGHTLIST_INIT(&light_list);
      LightInsertTail(&light_list, light_nodes.data(), n);
      uint64_t t0 = NowNs();
      light_c = LightDrainTail(&light_list);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);

      ModernInitNodes(modern_nodes.data(), n);
      modern_list.clear();
      ModernInsertTail(&modern_list, modern_nodes.data(), n);
      t0 = NowNs();
      modern_c = ModernDrainTail(&modern_list);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);

      KernelInitNodes(kernel_nodes.data(), n);
      INIT_LIST_HEAD(&kernel_head);
      KernelInsertTail(&kernel_head, kernel_nodes.data(), n);
      t0 = NowNs();
      kernel_c = KernelDrainTail(&kernel_head);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);
    }
    PrintRow("light_list", "drain_tail", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "drain_tail", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "drain_tail", n, Summarize(&kernel_s, kernel_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s, std_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0, std_c = 0;
    LightInitNodes(light_nodes.data(), n);
    LIGHTLIST_INIT(&light_list);
    LightInsertTail(&light_list, light_nodes.data(), n);
    ModernInitNodes(modern_nodes.data(), n);
    modern_list.clear();
    ModernInsertTail(&modern_list, modern_nodes.data(), n);
    KernelInitNodes(kernel_nodes.data(), n);
    INIT_LIST_HEAD(&kernel_head);
    KernelInsertTail(&kernel_head, kernel_nodes.data(), n);
    std_list.clear();
    StdInsertTail(&std_list, std_nodes.data(), n);
    for (int r = 0; r < total; ++r) {
      uint64_t t0 = NowNs();
      light_c = LightIterate(&light_list);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);

      t0 = NowNs();
      modern_c = ModernIterate(&modern_list);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);

      t0 = NowNs();
      kernel_c = KernelIterate(&kernel_head);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);

      t0 = NowNs();
      std_c = StdIterate(&std_list);
      dt = NowNs() - t0;
      if (r >= warmup) std_s.push_back(dt);
    }
    if (light_c != expected_sum || modern_c != expected_sum ||
        kernel_c != expected_sum || std_c != expected_sum) {
      std::fprintf(stderr,
                   "iterate checksum mismatch light=%llu modern=%llu "
                   "kernel=%llu std=%llu expected=%llu\n",
                   static_cast<unsigned long long>(light_c),
                   static_cast<unsigned long long>(modern_c),
                   static_cast<unsigned long long>(kernel_c),
                   static_cast<unsigned long long>(std_c),
                   static_cast<unsigned long long>(expected_sum));
      return 2;
    }
    PrintRow("light_list", "iterate", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "iterate", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "iterate", n, Summarize(&kernel_s, kernel_c));
    PrintRow("std::list", "iterate", n, Summarize(&std_s, std_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0;
    LightInitNodes(light_nodes.data(), n);
    LIGHTLIST_INIT(&light_list);
    LightInsertHead(&light_list, light_nodes.data(), n / 2);
    ModernInitNodes(modern_nodes.data(), n);
    modern_list.clear();
    ModernInsertHead(&modern_list, modern_nodes.data(), n / 2);
    KernelInitNodes(kernel_nodes.data(), n);
    INIT_LIST_HEAD(&kernel_head);
    KernelInsertHead(&kernel_head, kernel_nodes.data(), n / 2);
    for (int r = 0; r < total; ++r) {
      uint64_t t0 = NowNs();
      light_c = LightMembership(light_nodes.data(), n);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);

      t0 = NowNs();
      modern_c = ModernMembership(modern_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);

      t0 = NowNs();
      kernel_c = KernelMembership(kernel_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);
    }
    PrintRow("light_list", "membership", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "membership", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "membership", n, Summarize(&kernel_s, kernel_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0;
    for (int r = 0; r < total; ++r) {
      LightInitNodes(light_nodes.data(), n);
      LIGHTLIST_INIT(&light_list);
      LightInsertTail(&light_list, light_nodes.data(), n);
      uint64_t t0 = NowNs();
      light_c = LightLruTouch(&light_list, light_nodes.data(), n);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);

      ModernInitNodes(modern_nodes.data(), n);
      modern_list.clear();
      ModernInsertTail(&modern_list, modern_nodes.data(), n);
      t0 = NowNs();
      modern_c = ModernLruTouch(&modern_list, modern_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);

      KernelInitNodes(kernel_nodes.data(), n);
      INIT_LIST_HEAD(&kernel_head);
      KernelInsertTail(&kernel_head, kernel_nodes.data(), n);
      t0 = NowNs();
      kernel_c = KernelLruTouch(&kernel_head, kernel_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);
    }
    PrintRow("light_list", "lru_touch", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "lru_touch", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "lru_touch", n, Summarize(&kernel_s, kernel_c));
  }

  {
    std::vector<uint64_t> light_s, modern_s, kernel_s;
    uint64_t light_c = 0, modern_c = 0, kernel_c = 0;
    for (int r = 0; r < total; ++r) {
      LightInitNodes(light_nodes.data(), n);
      LIGHTLIST_INIT(&light_list);
      LightInsertHead(&light_list, light_nodes.data(), n);
      uint64_t t0 = NowNs();
      light_c = LightRemoveRand(&light_list, light_nodes.data(), n);
      uint64_t dt = NowNs() - t0;
      if (r >= warmup) light_s.push_back(dt);

      ModernInitNodes(modern_nodes.data(), n);
      modern_list.clear();
      ModernInsertHead(&modern_list, modern_nodes.data(), n);
      t0 = NowNs();
      modern_c = ModernRemoveRand(&modern_list, modern_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) modern_s.push_back(dt);

      KernelInitNodes(kernel_nodes.data(), n);
      INIT_LIST_HEAD(&kernel_head);
      KernelInsertHead(&kernel_head, kernel_nodes.data(), n);
      t0 = NowNs();
      kernel_c = KernelRemoveRand(kernel_nodes.data(), n);
      dt = NowNs() - t0;
      if (r >= warmup) kernel_s.push_back(dt);
    }
    PrintRow("light_list", "remove_rand", n, Summarize(&light_s, light_c));
    PrintRow("ll_modern", "remove_rand", n, Summarize(&modern_s, modern_c));
    PrintRow("list_head", "remove_rand", n, Summarize(&kernel_s, kernel_c));
  }

  std::fprintf(stderr,
               "\nnotes: ns/op = median wall / n; %d timed rounds after %d "
               "warmup. std::list allocates a heap node per insert.\n",
               rounds, warmup);
  return 0;
}
