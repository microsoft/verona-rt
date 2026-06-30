// THE vs Verona WSQ: Cilk-style fork/join topology comparison
//
// Uses the REAL WorkStealingQueue<4> from src/rt/sched/workstealingqueue.h.
// Both backends use identical execution model: work-first, self-count
// join counter, pre-allocated frame arenas.
//
// Build (from repo root):
//   c++ -std=c++20 -O3 -DNDEBUG -mcx16 \
//     -I. -Isrc/rt \
//     -I build_forkjoin_compare/_deps/snmalloc-src/src \
//     -DSNMALLOC_CHEAP_CHECKS -DSNMALLOC_USE_WAIT_ON_ADDRESS=1 \
//     -DMALLOC_USABLE_SIZE_QUALIFIER=const \
//     -DSNMALLOC_HAS_LINUX_FUTEX_H -DSNMALLOC_HAS_LINUX_RANDOM_H \
//     -DSNMALLOC_NO_REALLOCARR -DSNMALLOC_NO_REALLOCARRAY \
//     -DSNMALLOC_PLATFORM_HAS_GETENTROPY -DSNMALLOC_PTHREAD_ATFORK_WORKS \
//     test/perf/wsq_compare/cilk_wsq.cc -o cilk_wsq -pthread -latomic
//
// Run:
//   ./cilk_wsq --fib_n 42 --cutoff 20 --cores 10 --reps 30
//   ./cilk_wsq --fib_n 10 --cutoff 20 --cores 10 --reps 30 \
//     --uts_depth 10 --uts_cutoff 6 --uts_broad

#include "src/rt/sched/workstealingqueue.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using verona::rt::Work;
using verona::rt::WorkStealingQueue;
using verona::rt::ProbeOrder;

// ============================================================
// Frame: extends Work with fork/join state
// ============================================================

struct Frame : Work
{
  int n{0};
  int64_t result{0};
  std::atomic<int> join_counter{0};
  Frame* parent{nullptr};
  int child_index{0};
  int64_t child_results[2]{};

  Frame() : Work(nullptr) {}
};

// ============================================================
// Backend A: Cilk THE protocol (array-based deque + Dekker)
// ============================================================

namespace the_proto
{
  static constexpr size_t LTQ_SIZE = 2048;

  struct alignas(1024) Worker
  {
    alignas(64) Frame* ltq[LTQ_SIZE];
    alignas(64) std::atomic<int64_t> tail{0};
    alignas(64) std::atomic<int64_t> head{0};

    void push(Frame* f)
    {
      int64_t t = tail.load(std::memory_order_relaxed);
      ltq[t] = f;
      tail.store(t + 1, std::memory_order_release);
    }

    Frame* pop()
    {
      int64_t t = tail.load(std::memory_order_relaxed) - 1;
      tail.store(t, std::memory_order_seq_cst);
      int64_t h = head.load(std::memory_order_seq_cst);
      if (t > h)
        return ltq[t];
      if (t == h)
      {
        if (head.compare_exchange_strong(
              h, h + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
          tail.store(t + 1, std::memory_order_relaxed);
          return ltq[t];
        }
        tail.store(t + 1, std::memory_order_relaxed);
        return nullptr;
      }
      tail.store(t + 1, std::memory_order_relaxed);
      return nullptr;
    }

    Frame* steal()
    {
      int64_t h = head.load(std::memory_order_acquire);
      int64_t t = tail.load(std::memory_order_acquire);
      if (h >= t)
        return nullptr;
      Frame* f = ltq[h];
      if (head.compare_exchange_strong(
            h, h + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        return f;
      return nullptr;
    }

    void reset()
    {
      tail.store(0, std::memory_order_relaxed);
      head.store(0, std::memory_order_relaxed);
    }
  };
} // namespace the_proto

// ============================================================
// Backend B: Real Verona WorkStealingQueue<4>
// ============================================================

namespace wsq_proto
{
  struct alignas(1024) Worker
  {
    WorkStealingQueue<4> q;
    size_t victim_start{0};

    void push(Frame* f)
    {
      q.enqueue(static_cast<Work*>(f));
    }

    Frame* pop()
    {
      Work* w = q.pop_local();
      if (w)
        return static_cast<Frame*>(w);
      // Self-drain E→D
      w = q.refill_steal(q);
      return static_cast<Frame*>(w);
    }

    // Steal from another worker's queue
    Frame* steal_from(Worker& victim)
    {
      Work* w = q.template refill_steal<ProbeOrder::Heavy>(victim.q);
      return static_cast<Frame*>(w);
    }

    void reset()
    {
      // WSQ doesn't have a reset — just construct a new one
      // (for benchmarking, we create new Workers each run)
    }
  };
} // namespace wsq_proto

// ============================================================
// Fib serial baseline
// ============================================================

static int64_t fib_serial(int n)
{
  if (n < 2)
    return n;
  return fib_serial(n - 1) + fib_serial(n - 2);
}

// ============================================================
// Fib benchmark — templated over deque backend
// ============================================================

template<typename Worker>
struct FibBench
{
  int cutoff;
  int num_threads;
  std::vector<Worker*> workers;
  std::vector<Frame*> arenas;
  std::vector<size_t> arena_next;
  std::vector<int> victim_rotor;
  size_t arena_size{4'000'000};
  std::atomic<bool> done{false};

  int next_victim(int id)
  {
    int v = victim_rotor[id];
    do { v = (v + 1) % num_threads; } while (v == id);
    victim_rotor[id] = v;
    return v;
  }

  Frame* alloc_frame(int id)
  {
    size_t& nx = arena_next[id];
    assert(nx < arena_size);
    Frame* f = &arenas[id][nx++];
    f->next_in_queue = nullptr;
    f->result = 0;
    f->join_counter.store(0, std::memory_order_relaxed);
    return f;
  }

  void join(Frame* f, int id)
  {
    f->result = f->child_results[0] + f->child_results[1];
    if (f->parent)
    {
      f->parent->child_results[f->child_index] = f->result;
      if (f->parent->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)
        join(f->parent, id);
    }
  }

  Frame* try_steal(int id)
  {
    for (int attempt = 0; attempt < num_threads - 1; attempt++)
    {
      int v = next_victim(id);
      Frame* stolen = nullptr;
      if constexpr (std::is_same_v<Worker, wsq_proto::Worker>)
        stolen = workers[id]->steal_from(*workers[v]);
      else
        stolen = workers[v]->steal();
      if (stolen)
        return stolen;
    }
    return nullptr;
  }

  void process(Frame* f, int id)
  {
    if (f->n < cutoff)
    {
      f->result = fib_serial(f->n);
      if (f->parent)
      {
        f->parent->child_results[f->child_index] = f->result;
        if (f->parent->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)
          join(f->parent, id);
      }
      return;
    }

    f->join_counter.store(3, std::memory_order_relaxed); // self + 2 children

    Frame* child_a = alloc_frame(id);
    child_a->n = f->n - 1;
    child_a->parent = f;
    child_a->child_index = 0;

    Frame* child_b = alloc_frame(id);
    child_b->n = f->n - 2;
    child_b->parent = f;
    child_b->child_index = 1;

    // Push child_a (work-first: continue into child_b)
    workers[id]->push(child_a);

    // Execute child_b inline
    process(child_b, id);

    // Try to reclaim child_a
    Frame* popped = workers[id]->pop();
    if (popped == child_a)
    {
      process(child_a, id);
    }
    else
    {
      if (popped)
        process(popped, id);

      // Help until child_a completes
      while (f->join_counter.load(std::memory_order_acquire) > 1)
      {
        Frame* work = workers[id]->pop();
        if (!work)
          work = try_steal(id);
        if (work)
          process(work, id);
      }
    }

    // Remove self-count
    if (f->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)
      join(f, id);
  }

  void worker_loop(int id)
  {
    while (!done.load(std::memory_order_relaxed))
    {
      Frame* f = workers[id]->pop();
      if (f)
      {
        process(f, id);
        continue;
      }
      Frame* stolen = try_steal(id);
      if (stolen)
      {
        process(stolen, id);
        continue;
      }
      // No work found — yield and retry (don't exit)
      std::this_thread::yield();
    }
  }

  double run_once(int fib_n, int cut, int nthreads)
  {
    cutoff = cut;
    num_threads = nthreads;
    done.store(false);

    workers.resize(nthreads);
    arenas.resize(nthreads);
    arena_next.assign(nthreads, 0);
    victim_rotor.resize(nthreads);
    for (int i = 0; i < nthreads; i++)
    {
      workers[i] = new Worker();
      arenas[i] = new Frame[arena_size]();
      victim_rotor[i] = i;
    }

    Frame* root = alloc_frame(0);
    root->n = fib_n;
    root->parent = nullptr;
    workers[0]->push(root);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < nthreads; i++)
      threads.emplace_back(&FibBench::worker_loop, this, i);

    Frame* first = workers[0]->pop();
    if (first)
      process(first, 0);

    done.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < nthreads; i++)
    {
      delete workers[i];
      delete[] arenas[i];
    }
    workers.clear();
    arenas.clear();

    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
};

// ============================================================
// Statistics
// ============================================================

struct Stats
{
  double median, min, q1, q3;
};

static Stats compute_stats(std::vector<double>& v)
{
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  return {
    (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) / 2.0 : v[n / 2],
    v[0],
    v[n / 4],
    v[3 * n / 4]};
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
  int fib_n = 35;
  int cutoff = 20;
  int num_threads = 2;
  int reps = 30;

  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--fib_n") && i + 1 < argc)
      fib_n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cutoff") && i + 1 < argc)
      cutoff = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cores") && i + 1 < argc)
      num_threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc)
      reps = atoi(argv[++i]);
  }

  printf("=== THE vs Verona WSQ: Cilk-style Fork/Join ===\n");
  printf("fib(%d) cutoff=%d, cores=%d, reps=%d (interleaved)\n",
         fib_n, cutoff, num_threads, reps);
  printf("D[4] backend: real WorkStealingQueue<4> from verona-rt\n\n");

  // Serial
  std::vector<double> serial_times;
  int64_t expected = 0;
  for (int r = 0; r < std::min(reps, 10); r++)
  {
    auto t0 = std::chrono::high_resolution_clock::now();
    expected = fib_serial(fib_n);
    auto t1 = std::chrono::high_resolution_clock::now();
    serial_times.push_back(
      std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  auto ser = compute_stats(serial_times);
  printf("Serial:   median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         ser.median, ser.min, ser.q1, ser.q3);

  // Interleaved runs
  FibBench<the_proto::Worker> the_bench;
  FibBench<wsq_proto::Worker> wsq_bench;
  std::vector<double> the_times, wsq_times;

  for (int r = 0; r < reps; r++)
  {
    the_times.push_back(the_bench.run_once(fib_n, cutoff, num_threads));
    wsq_times.push_back(wsq_bench.run_once(fib_n, cutoff, num_threads));
  }

  auto the_s = compute_stats(the_times);
  auto wsq_s = compute_stats(wsq_times);
  printf("THE:      median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         the_s.median, the_s.min, the_s.q1, the_s.q3);
  printf("WSQ:      median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         wsq_s.median, wsq_s.min, wsq_s.q1, wsq_s.q3);

  printf("\nWSQ/THE median: %.3fx  min: %.3fx  (>1 = WSQ faster)\n",
         the_s.median / wsq_s.median, the_s.min / wsq_s.min);

  return 0;
}
