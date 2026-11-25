// try_push/try_pop throughput: P producers, C consumers, spin on failure. Build once per header variant.
#include <rigtorp/MPMCQueue.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
int main(int argc, char **argv) {
  int P = atoi(argv[1]), C = atoi(argv[2]);
  long N = atol(argv[3]);                      // items per producer
  rigtorp::MPMCQueue<long> q(atoi(argv[4]));   // capacity
  std::atomic<int> ready{0};
  std::atomic<long> sum{0};
  long total = N * P;
  std::atomic<long> popped{0};
  std::vector<std::thread> ts;
  for (int p = 0; p < P; ++p)
    ts.emplace_back([&] {
      ready.fetch_add(1); while (ready.load() < P + C) {}
      for (long i = 1; i <= N; ++i) while (!q.try_push(i)) {}
    });
  for (int c = 0; c < C; ++c)
    ts.emplace_back([&] {
      ready.fetch_add(1); while (ready.load() < P + C) {}
      long s = 0, v;
      while (popped.load(std::memory_order_relaxed) < total)
        if (q.try_pop(v)) { s += v; popped.fetch_add(1, std::memory_order_relaxed); }
      sum.fetch_add(s);
    });
  while (ready.load() < P + C) {}
  auto t0 = std::chrono::steady_clock::now();
  for (auto &t : ts) t.join();
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (sum.load() != P * (N * (N + 1) / 2)) { puts("CHECKSUM MISMATCH"); return 1; }
  printf("%.2f\n", total / s / 1e6);         // Mops/s
}
