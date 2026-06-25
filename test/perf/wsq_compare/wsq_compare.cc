// WSQ Topology Isolation Benchmark
//
// Compares verona-rt's 3-stack multi-level WSQ against a standard
// Chase-Lev work-stealing deque on the same fork/join workload (fib).
//
// Removes ALL allocation from the hot path: tasks come from per-thread
// pre-allocated arenas. Both backends use an identical flat work-loop
// (no recursion) with continuation-passing:
//   - Leaf: compute serial fib, signal parent
//   - Fork: push two children to the queue
//   - Join: when both children done, compute sum, signal grandparent
//
// This isolates the queue topology/protocol differences from memory
// allocation, call-stack layout, and recursion strategy.
//
// Build (standalone, no snmalloc dependency):
//   c++ -std=c++20 -O3 -DNDEBUG -mcx16 \
//     test/perf/wsq_compare/wsq_compare.cc \
//     -o build_forkjoin_compare/wsq_compare -pthread -latomic

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

// ============================================================
// Chase-Lev Deque (standard single-level work-stealing deque)
// Reference: "Dynamic Circular Work-Stealing Deque" (Chase & Lev, 2005)
// ============================================================

namespace chase_lev
{
  template<typename T, size_t LogCapacity = 17>
  class Deque
  {
    static constexpr size_t CAPACITY = 1ULL << LogCapacity;
    static constexpr size_t MASK = CAPACITY - 1;

    alignas(64) std::atomic<int64_t> top_{0};
    alignas(64) std::atomic<int64_t> bottom_{0};
    alignas(64) T* buffer_[CAPACITY]{};

  public:
    void push(T* item)
    {
      int64_t b = bottom_.load(std::memory_order_relaxed);
      buffer_[b & MASK] = item;
      std::atomic_thread_fence(std::memory_order_release);
      bottom_.store(b + 1, std::memory_order_relaxed);
    }

    T* pop()
    {
      int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
      bottom_.store(b, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t t = top_.load(std::memory_order_relaxed);

      if (t <= b)
      {
        T* item = buffer_[b & MASK];
        if (t == b)
        {
          if (!top_.compare_exchange_strong(
                t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            item = nullptr;
          bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return item;
      }
      else
      {
        bottom_.store(b + 1, std::memory_order_relaxed);
        return nullptr;
      }
    }

    T* steal()
    {
      int64_t t = top_.load(std::memory_order_acquire);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t b = bottom_.load(std::memory_order_acquire);

      if (t < b)
      {
        T* item = buffer_[t & MASK];
        if (!top_.compare_exchange_strong(
              t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
          return nullptr;
        return item;
      }
      return nullptr;
    }
  };
} // namespace chase_lev

// ============================================================
// Verona-style 3-stack WSQ (standalone copy, no deps)
// ============================================================

namespace verona_wsq
{
  struct Work
  {
    Work* next_in_queue{nullptr};
  };

  template<size_t N>
  class WrapIndex
  {
    size_t idx_;

  public:
    constexpr WrapIndex(size_t i = 0) : idx_(i % N) {}
    operator size_t() const { return idx_; }
    WrapIndex& operator++()
    {
      idx_ = (idx_ + 1) % N;
      return *this;
    }
    WrapIndex operator++(int)
    {
      auto old = *this;
      ++*this;
      return old;
    }
    WrapIndex& operator--()
    {
      idx_ = (idx_ + N - 1) % N;
      return *this;
    }
    WrapIndex operator--(int)
    {
      auto old = *this;
      --*this;
      return old;
    }
  };

  enum class ProbeOrder : uint8_t
  {
    Heavy,
    Light,
  };

  template<size_t N = 4>
  class alignas(64) WorkStealingQueue
  {
    alignas(64) std::atomic<Work*> inbox_head{nullptr};
    alignas(64) std::atomic<Work*> dequeue_heads[N]{};
    alignas(64) std::atomic<Work*> enqueue_head{nullptr};
    alignas(64) WrapIndex<N> dequeue_index{N - 1};
    WrapIndex<N> steal_index{};

    static void push(std::atomic<Work*>& head, Work* w)
    {
      Work* old = head.load(std::memory_order_relaxed);
      while (true)
      {
        w->next_in_queue = old;
        if (head.compare_exchange_weak(
              old, w, std::memory_order_release, std::memory_order_relaxed))
          return;
      }
    }

    template<std::memory_order Order = std::memory_order_acquire>
    static Work* pop_all(std::atomic<Work*>& head)
    {
      if (head.load(std::memory_order_relaxed) == nullptr)
        return nullptr;
      return head.exchange(nullptr, Order);
    }

    static void owner_push(std::atomic<Work*>& head, Work* chain)
    {
      head.store(chain, std::memory_order_relaxed);
    }

    static Work* split_head(Work*& bucket)
    {
      assert(bucket != nullptr);
      return std::exchange(bucket, bucket->next_in_queue);
    }

    static Work* owner_pop_one(std::atomic<Work*>& head)
    {
      Work* chain = pop_all<std::memory_order_relaxed>(head);
      if (chain == nullptr)
        return nullptr;
      Work* result = split_head(chain);
      owner_push(head, chain);
      return result;
    }

    static void stripe_chain_onto(
      Work* chain, std::array<Work*, N>& buckets, WrapIndex<N>& k)
    {
      while (chain != nullptr)
      {
        Work* top = split_head(chain);
        top->next_in_queue = std::exchange(buckets[k], top);
        ++k;
      }
    }

    void stripe_into_dequeue(std::array<Work*, N>& buckets)
    {
      std::atomic_thread_fence(std::memory_order_release);
      for (size_t i = 0; i < N; ++i)
      {
        if (buckets[i] == nullptr)
          continue;
        owner_push(dequeue_heads[i], buckets[i]);
      }
    }

    template<ProbeOrder Order>
    Work* probe_victim(WorkStealingQueue& victim)
    {
      if (Work* c = pop_all(victim.inbox_head))
        return c;
      if constexpr (Order == ProbeOrder::Heavy)
      {
        if (Work* c = pop_all(victim.enqueue_head))
          return c;
        for (size_t i = 0; i < N; ++i)
          if (Work* c = pop_all(victim.dequeue_heads[++steal_index]))
            return c;
      }
      else
      {
        for (size_t i = 0; i < N; ++i)
          if (Work* c = pop_all(victim.dequeue_heads[++steal_index]))
            return c;
        if (Work* c = pop_all(victim.enqueue_head))
          return c;
      }
      return nullptr;
    }

  public:
    constexpr WorkStealingQueue() = default;

    void enqueue(Work* w)
    {
      push(enqueue_head, w);
    }

    void enqueue_front(Work* w)
    {
      push(inbox_head, w);
    }

    Work* pop_local()
    {
      for (size_t attempt = 0; attempt < N; ++attempt)
      {
        Work* w = owner_pop_one(dequeue_heads[dequeue_index--]);
        if (w != nullptr)
          return w;
      }
      return nullptr;
    }

    template<ProbeOrder Order = ProbeOrder::Light>
    Work* refill_steal(WorkStealingQueue& victim)
    {
      Work* stolen =
        (&victim == this) ? nullptr : probe_victim<Order>(victim);
      Work* echain = pop_all(enqueue_head);
      Work* ichain = pop_all(inbox_head);

      if (stolen == nullptr && echain == nullptr && ichain == nullptr)
        return nullptr;

      std::array<Work*, N> buckets{};
      WrapIndex<N> k{};

      if (echain != nullptr)
        stripe_chain_onto(echain, buckets, k);
      if (stolen != nullptr)
        stripe_chain_onto(stolen, buckets, k);
      if (ichain != nullptr)
        stripe_chain_onto(ichain, buckets, k);

      k--;
      assert(buckets[k] != nullptr);
      Work* head = split_head(buckets[k]);
      stripe_into_dequeue(buckets);
      dequeue_index = k;
      return head;
    }

    bool is_empty()
    {
      if (inbox_head.load(std::memory_order_relaxed) != nullptr)
        return false;
      if (enqueue_head.load(std::memory_order_relaxed) != nullptr)
        return false;
      for (size_t i = 0; i < N; ++i)
        if (dequeue_heads[i].load(std::memory_order_relaxed) != nullptr)
          return false;
      return true;
    }
  };
} // namespace verona_wsq

// ============================================================
// Task structure (continuation-passing, fully iterative)
// ============================================================

struct FibTask
{
  // Queue linkage for verona WSQ (unused by Chase-Lev)
  verona_wsq::Work work;

  int n;
  int64_t result;
  bool forked{false}; // true after process_task forks this task

  // Parent continuation: when children_remaining hits 0, re-enqueue parent
  FibTask* parent;
  int child_index; // which slot in parent->child_results[] to fill

  std::atomic<int> children_remaining{0};
  int64_t child_results[2];
};

// Per-thread arena
struct TaskArena
{
  std::vector<FibTask> pool;
  size_t next{0};

  TaskArena() = default;
  explicit TaskArena(size_t capacity) : pool(capacity) {}

  FibTask* alloc()
  {
    assert(next < pool.size());
    FibTask* t = &pool[next++];
    t->work.next_in_queue = nullptr;
    t->children_remaining.store(0, std::memory_order_relaxed);
    t->forked = false;
    t->result = 0;
    return t;
  }

  void reset()
  {
    next = 0;
  }
};

// ============================================================
// Serial fib (baseline)
// ============================================================

static int64_t fib_serial(int n)
{
  if (n < 2)
    return n;
  return fib_serial(n - 1) + fib_serial(n - 2);
}

// ============================================================
// Backend 1: Chase-Lev
// ============================================================

namespace bench_cl
{
  struct ThreadState
  {
    chase_lev::Deque<FibTask>* deque;
    TaskArena* arena;
  };

  static int g_cutoff;
  static int g_num_threads;
  static std::vector<ThreadState> g_threads;
  static std::atomic<bool> g_done{false};
  static thread_local int tl_id;

  // Process a single task (non-recursive). May push children to queue.
  // Returns a "ready" parent task (if this was the last child), or nullptr.
  static FibTask* process_task(FibTask* t)
  {
    if (t->n < g_cutoff)
    {
      // Leaf: compute serial result
      t->result = fib_serial(t->n);

      // Signal parent
      if (t->parent)
      {
        t->parent->child_results[t->child_index] = t->result;
        if (t->parent->children_remaining.fetch_sub(
              1, std::memory_order_acq_rel) == 1)
        {
          // Parent ready — return it for immediate execution
          return t->parent;
        }
      }
      return nullptr;
    }

    // Fork: create two children, push one, return the other for immediate exec
    auto* arena = g_threads[tl_id].arena;
    auto* deque = g_threads[tl_id].deque;

    t->forked = true;
    t->children_remaining.store(2, std::memory_order_relaxed);

    FibTask* child_a = arena->alloc();
    child_a->n = t->n - 1;
    child_a->parent = t;
    child_a->child_index = 0;

    FibTask* child_b = arena->alloc();
    child_b->n = t->n - 2;
    child_b->parent = t;
    child_b->child_index = 1;

    // Push child_a (stealable), return child_b for local execution
    deque->push(child_a);
    return child_b;
  }

  // Process a "join" (parent whose children completed)
  static FibTask* process_join(FibTask* t)
  {
    t->result = t->child_results[0] + t->child_results[1];

    if (t->parent)
    {
      t->parent->child_results[t->child_index] = t->result;
      if (t->parent->children_remaining.fetch_sub(
            1, std::memory_order_acq_rel) == 1)
        return t->parent;
    }
    return nullptr;
  }

  static void worker_loop(int id)
  {
    tl_id = id;
    auto* deque = g_threads[id].deque;

    while (!g_done.load(std::memory_order_relaxed))
    {
      // Try local pop
      FibTask* t = deque->pop();

      // Try stealing
      if (!t)
      {
        for (int i = 0; i < g_num_threads; i++)
        {
          int victim = (id + 1 + i) % g_num_threads;
          if (victim == id)
            continue;
          t = g_threads[victim].deque->steal();
          if (t)
            break;
        }
      }

      if (!t)
        continue;

      // Execute task chain (follow continuations without recursion)
      while (t)
      {
        if (t->forked && t->children_remaining.load(std::memory_order_relaxed) == 0)
        {
          // This is a join continuation
          t = process_join(t);
        }
        else
        {
          t = process_task(t);
        }
      }
    }
  }

  static double run(int fib_n, int cutoff, int num_threads, int64_t* result)
  {
    g_cutoff = cutoff;
    g_num_threads = num_threads;
    g_done.store(false, std::memory_order_relaxed);

    // Arena size: fib(n-cutoff+2) internal nodes × 2 children + margin
    size_t arena_size = 8'000'000;
    g_threads.resize(num_threads);
    for (int i = 0; i < num_threads; i++)
    {
      g_threads[i].deque = new chase_lev::Deque<FibTask>();
      g_threads[i].arena = new TaskArena(arena_size);
    }

    // Root task
    FibTask* root = g_threads[0].arena->alloc();
    root->n = fib_n;
    root->parent = nullptr;
    root->child_index = 0;
    g_threads[0].deque->push(root);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < num_threads; i++)
      threads.emplace_back(worker_loop, i);

    // Thread 0 works until root is complete
    tl_id = 0;
    auto* deque = g_threads[0].deque;

    while (root->result == 0 || root->children_remaining.load(std::memory_order_acquire) != 0)
    {
      FibTask* t = deque->pop();
      if (!t)
      {
        for (int i = 0; i < num_threads; i++)
        {
          int victim = (0 + 1 + i) % num_threads;
          if (victim == 0)
            continue;
          t = g_threads[victim].deque->steal();
          if (t)
            break;
        }
      }
      if (!t)
        continue;

      while (t)
      {
        if (t->forked && t->children_remaining.load(std::memory_order_relaxed) == 0)
          t = process_join(t);
        else
          t = process_task(t);
      }
    }

    // Handle the root join if needed
    if (root->n >= g_cutoff)
      root->result = root->child_results[0] + root->child_results[1];

    auto t1 = std::chrono::high_resolution_clock::now();

    g_done.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    *result = root->result;

    for (int i = 0; i < num_threads; i++)
    {
      delete g_threads[i].deque;
      delete g_threads[i].arena;
    }
    g_threads.clear();
    return ms;
  }
} // namespace bench_cl

// ============================================================
// Backend 2: Verona WSQ
// ============================================================

namespace bench_vr
{
  struct ThreadState
  {
    verona_wsq::WorkStealingQueue<4>* queue;
    TaskArena* arena;
  };

  static int g_cutoff;
  static int g_num_threads;
  static std::vector<ThreadState> g_threads;
  static std::atomic<bool> g_done{false};
  static thread_local int tl_id;

  static FibTask* get_work(int id)
  {
    auto* q = g_threads[id].queue;

    // Hot path: pop from D
    verona_wsq::Work* w = q->pop_local();
    if (w)
      return reinterpret_cast<FibTask*>(w);

    // Self-drain: E+I → D
    w = q->refill_steal(*q);
    if (w)
      return reinterpret_cast<FibTask*>(w);

    // Steal from others
    for (int i = 0; i < g_num_threads; i++)
    {
      int victim = (id + 1 + i) % g_num_threads;
      if (victim == id)
        continue;
      w = q->template refill_steal<verona_wsq::ProbeOrder::Heavy>(
        *g_threads[victim].queue);
      if (w)
        return reinterpret_cast<FibTask*>(w);
    }
    return nullptr;
  }

  static FibTask* process_task(FibTask* t)
  {
    if (t->n < g_cutoff)
    {
      t->result = fib_serial(t->n);
      if (t->parent)
      {
        t->parent->child_results[t->child_index] = t->result;
        if (t->parent->children_remaining.fetch_sub(
              1, std::memory_order_acq_rel) == 1)
          return t->parent;
      }
      return nullptr;
    }

    auto* arena = g_threads[tl_id].arena;
    auto* q = g_threads[tl_id].queue;

    t->forked = true;
    t->children_remaining.store(2, std::memory_order_relaxed);

    FibTask* child_a = arena->alloc();
    child_a->n = t->n - 1;
    child_a->parent = t;
    child_a->child_index = 0;

    FibTask* child_b = arena->alloc();
    child_b->n = t->n - 2;
    child_b->parent = t;
    child_b->child_index = 1;

    // Push child_a to WSQ (stealable), return child_b for local exec
    q->enqueue(&child_a->work);
    return child_b;
  }

  static FibTask* process_join(FibTask* t)
  {
    t->result = t->child_results[0] + t->child_results[1];
    if (t->parent)
    {
      t->parent->child_results[t->child_index] = t->result;
      if (t->parent->children_remaining.fetch_sub(
            1, std::memory_order_acq_rel) == 1)
        return t->parent;
    }
    return nullptr;
  }

  static void worker_loop(int id)
  {
    tl_id = id;
    while (!g_done.load(std::memory_order_relaxed))
    {
      FibTask* t = get_work(id);
      if (!t)
        continue;

      while (t)
      {
        if (t->forked && t->children_remaining.load(std::memory_order_relaxed) == 0)
          t = process_join(t);
        else
          t = process_task(t);
      }
    }
  }

  static double run(int fib_n, int cutoff, int num_threads, int64_t* result)
  {
    g_cutoff = cutoff;
    g_num_threads = num_threads;
    g_done.store(false, std::memory_order_relaxed);

    size_t arena_size = 8'000'000;
    g_threads.resize(num_threads);
    for (int i = 0; i < num_threads; i++)
    {
      g_threads[i].queue = new verona_wsq::WorkStealingQueue<4>();
      g_threads[i].arena = new TaskArena(arena_size);
    }

    FibTask* root = g_threads[0].arena->alloc();
    root->n = fib_n;
    root->parent = nullptr;
    root->child_index = 0;
    g_threads[0].queue->enqueue(&root->work);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < num_threads; i++)
      threads.emplace_back(worker_loop, i);

    tl_id = 0;
    while (root->result == 0 || root->children_remaining.load(std::memory_order_acquire) != 0)
    {
      FibTask* t = get_work(0);
      if (!t)
        continue;

      while (t)
      {
        if (t->forked && t->children_remaining.load(std::memory_order_relaxed) == 0)
          t = process_join(t);
        else
          t = process_task(t);
      }
    }

    if (root->n >= g_cutoff)
      root->result = root->child_results[0] + root->child_results[1];

    auto t1 = std::chrono::high_resolution_clock::now();

    g_done.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    *result = root->result;

    for (int i = 0; i < num_threads; i++)
    {
      delete g_threads[i].queue;
      delete g_threads[i].arena;
    }
    g_threads.clear();
    return ms;
  }
} // namespace bench_vr

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
  int fib_n = 42;
  int cutoff = 20;
  int num_threads = 2;
  int reps = 5;

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

  printf("=== WSQ Topology Isolation Benchmark ===\n");
  printf("fib(%d) cutoff=%d, cores=%d, reps=%d\n", fib_n, cutoff, num_threads, reps);
  printf("Task model: iterative continuation-passing (no recursion)\n");
  printf("Allocation: pre-allocated arena (no malloc in hot path)\n\n");

  // Serial baseline
  int64_t expected = 0;
  double serial_best = 1e18;
  for (int r = 0; r < reps; r++)
  {
    auto t0 = std::chrono::high_resolution_clock::now();
    expected = fib_serial(fib_n);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < serial_best)
      serial_best = ms;
  }
  printf("Serial:        %8.1f ms  (result=%ld)\n", serial_best, expected);

  // Chase-Lev
  int64_t cl_result = 0;
  double cl_best = 1e18;
  for (int r = 0; r < reps; r++)
  {
    double ms = bench_cl::run(fib_n, cutoff, num_threads, &cl_result);
    if (ms < cl_best)
      cl_best = ms;
  }
  printf(
    "Chase-Lev:     %8.1f ms  (result=%ld, speedup=%.2fx)\n",
    cl_best, cl_result, serial_best / cl_best);

  // Verona WSQ
  int64_t vr_result = 0;
  double vr_best = 1e18;
  for (int r = 0; r < reps; r++)
  {
    double ms = bench_vr::run(fib_n, cutoff, num_threads, &vr_result);
    if (ms < vr_best)
      vr_best = ms;
  }
  printf(
    "Verona WSQ:    %8.1f ms  (result=%ld, speedup=%.2fx)\n",
    vr_best, vr_result, serial_best / vr_best);

  printf("\n--- Queue comparison ---\n");
  printf("Verona / Chase-Lev: %.3fx (>1 = Verona faster)\n", cl_best / vr_best);

  // Correctness
  int ok = 1;
  if (cl_result != expected)
  {
    printf("ERROR: Chase-Lev result=%ld expected=%ld\n", cl_result, expected);
    ok = 0;
  }
  if (vr_result != expected)
  {
    printf("ERROR: Verona result=%ld expected=%ld\n", vr_result, expected);
    ok = 0;
  }
  if (ok)
    printf("All results correct.\n");

  return ok ? 0 : 1;
}
