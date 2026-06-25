// Flat-task steal benchmark: steal-all vs steal-one
//
// Demonstrates the contention advantage of Verona's pop-all (exchange)
// over Chase-Lev's steal-one (CAS on top) when work items are small
// and uniform — i.e., when Cilk's "big item at the top" invariant
// doesn't hold.
//
// Workload: One producer core generates N small uniform tasks.
// All other cores are pure thieves. Each task does a fixed amount
// of trivial work (no tree structure, no hierarchy).
//
// What this measures:
//   - Steal throughput under contention (K thieves → 1 victim)
//   - How quickly work migrates from a hot producer to idle cores
//   - The cost of repeated steal attempts vs grab-all-at-once
//
// Build:
//   c++ -std=c++20 -O3 -DNDEBUG -mcx16 \
//     flat_steal.cc -o flat_steal -pthread -latomic

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
// Chase-Lev Deque (steal-one)
// ============================================================

namespace chase_lev
{
  template<typename T, size_t LogCapacity = 20>
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
      bottom_.store(b + 1, std::memory_order_relaxed);
      return nullptr;
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
// Verona-style 3-stack WSQ (steal-all)
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
// Task definition (flat, uniform, no hierarchy)
// ============================================================

struct Task
{
  verona_wsq::Work work; // WSQ linkage (first member for reinterpret_cast)
  uint64_t payload;      // trivial work token
};

// Trivial "work" — enough to prevent the compiler from optimizing away
// the task execution, but small enough that queue overhead dominates.
static inline uint64_t do_work(uint64_t x)
{
  // ~5-10ns of ALU work
  x ^= x >> 17;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 31;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 32;
  return x;
}

// ============================================================
// Scenario: Producer-consumer (1 producer, K-1 thieves)
//
// Producer pushes tasks in batches. Thieves steal continuously.
// Measures: total time to process N tasks across all cores.
// ============================================================

namespace bench_cl_flat
{
  static std::atomic<uint64_t> g_sum{0};
  static std::atomic<size_t> g_tasks_done{0};
  static std::atomic<bool> g_producer_done{false};
  static std::atomic<bool> g_stop{false};

  static void thief_loop(
    int id, int num_threads, std::vector<chase_lev::Deque<Task>*>& deques)
  {
    uint64_t local_sum = 0;
    size_t local_done = 0;

    while (!g_stop.load(std::memory_order_relaxed))
    {
      // Try own deque first (has stolen work from previous iterations)
      Task* t = deques[id]->pop();
      if (!t)
      {
        // Steal from any other thread (primarily the producer, thread 0)
        for (int i = 0; i < num_threads; i++)
        {
          int victim = (id + i) % num_threads;
          if (victim == id)
            continue;
          t = deques[victim]->steal();
          if (t)
            break;
        }
      }

      if (t)
      {
        local_sum += do_work(t->payload);
        local_done++;
      }
    }

    g_sum.fetch_add(local_sum, std::memory_order_relaxed);
    g_tasks_done.fetch_add(local_done, std::memory_order_relaxed);
  }

  static double run(
    size_t num_tasks, int batch_size, int num_threads, size_t* tasks_completed)
  {
    g_sum.store(0, std::memory_order_relaxed);
    g_tasks_done.store(0, std::memory_order_relaxed);
    g_producer_done.store(false, std::memory_order_relaxed);
    g_stop.store(false, std::memory_order_relaxed);

    // Pre-allocate all tasks
    std::vector<Task> pool(num_tasks);
    for (size_t i = 0; i < num_tasks; i++)
      pool[i].payload = i + 1;

    std::vector<chase_lev::Deque<Task>*> deques(num_threads);
    for (int i = 0; i < num_threads; i++)
      deques[i] = new chase_lev::Deque<Task>();

    auto t0 = std::chrono::high_resolution_clock::now();

    // Launch thieves
    std::vector<std::thread> threads;
    for (int i = 1; i < num_threads; i++)
      threads.emplace_back(thief_loop, i, num_threads, std::ref(deques));

    // Producer: push all tasks in batches, also process own deque
    uint64_t producer_sum = 0;
    size_t producer_done = 0;
    size_t pushed = 0;

    while (pushed < num_tasks)
    {
      // Push a batch
      size_t batch_end = std::min(pushed + (size_t)batch_size, num_tasks);
      for (size_t i = pushed; i < batch_end; i++)
        deques[0]->push(&pool[i]);
      pushed = batch_end;

      // Process some of our own work too
      for (int j = 0; j < batch_size / 2; j++)
      {
        Task* t = deques[0]->pop();
        if (!t)
          break;
        producer_sum += do_work(t->payload);
        producer_done++;
      }
    }

    // Producer finishes pushing; drain remaining local work
    while (true)
    {
      Task* t = deques[0]->pop();
      if (!t)
        break;
      producer_sum += do_work(t->payload);
      producer_done++;
    }

    // Wait for all tasks to be processed
    // (some tasks may still be in-flight on thieves)
    g_producer_done.store(true, std::memory_order_release);

    // Brief spin to let thieves finish current work
    for (int spin = 0; spin < 10000; spin++)
    {
      bool all_empty = true;
      for (int i = 0; i < num_threads; i++)
      {
        // Check if any deque still has work
        if (deques[i]->pop() != nullptr)
        {
          all_empty = false;
          // We accidentally popped — count it
          producer_sum += 1; // approximate
          producer_done++;
        }
      }
      if (all_empty)
        break;
    }

    g_stop.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    *tasks_completed =
      producer_done + g_tasks_done.load(std::memory_order_relaxed);

    for (int i = 0; i < num_threads; i++)
      delete deques[i];

    return ms;
  }
} // namespace bench_cl_flat

// ============================================================
// Verona WSQ flat benchmark
// ============================================================

namespace bench_vr_flat
{
  static std::atomic<uint64_t> g_sum{0};
  static std::atomic<size_t> g_tasks_done{0};
  static std::atomic<bool> g_stop{false};

  struct ThreadState
  {
    verona_wsq::WorkStealingQueue<4>* queue;
  };

  static Task* get_work(
    int id, int num_threads, std::vector<ThreadState>& threads)
  {
    auto* q = threads[id].queue;

    verona_wsq::Work* w = q->pop_local();
    if (w)
      return reinterpret_cast<Task*>(w);

    w = q->refill_steal(*q);
    if (w)
      return reinterpret_cast<Task*>(w);

    for (int i = 0; i < num_threads; i++)
    {
      int victim = (id + i) % num_threads;
      if (victim == id)
        continue;
      w = q->template refill_steal<verona_wsq::ProbeOrder::Heavy>(
        *threads[victim].queue);
      if (w)
        return reinterpret_cast<Task*>(w);
    }
    return nullptr;
  }

  static void thief_loop(
    int id, int num_threads, std::vector<ThreadState>& threads)
  {
    uint64_t local_sum = 0;
    size_t local_done = 0;

    while (!g_stop.load(std::memory_order_relaxed))
    {
      Task* t = get_work(id, num_threads, threads);
      if (t)
      {
        local_sum += do_work(t->payload);
        local_done++;
      }
    }

    g_sum.fetch_add(local_sum, std::memory_order_relaxed);
    g_tasks_done.fetch_add(local_done, std::memory_order_relaxed);
  }

  static double run(
    size_t num_tasks, int batch_size, int num_threads, size_t* tasks_completed)
  {
    g_sum.store(0, std::memory_order_relaxed);
    g_tasks_done.store(0, std::memory_order_relaxed);
    g_stop.store(false, std::memory_order_relaxed);

    std::vector<Task> pool(num_tasks);
    for (size_t i = 0; i < num_tasks; i++)
      pool[i].payload = i + 1;

    std::vector<ThreadState> threads(num_threads);
    for (int i = 0; i < num_threads; i++)
      threads[i].queue = new verona_wsq::WorkStealingQueue<4>();

    auto t0 = std::chrono::high_resolution_clock::now();

    // Launch thieves
    std::vector<std::thread> workers;
    for (int i = 1; i < num_threads; i++)
      workers.emplace_back(thief_loop, i, num_threads, std::ref(threads));

    // Producer: push all tasks, also consume locally
    auto* q = threads[0].queue;
    uint64_t producer_sum = 0;
    size_t producer_done = 0;
    size_t pushed = 0;

    while (pushed < num_tasks)
    {
      size_t batch_end = std::min(pushed + (size_t)batch_size, num_tasks);
      for (size_t i = pushed; i < batch_end; i++)
        q->enqueue(&pool[i].work);
      pushed = batch_end;

      // Process some locally
      for (int j = 0; j < batch_size / 2; j++)
      {
        Task* t = get_work(0, num_threads, threads);
        if (!t)
          break;
        producer_sum += do_work(t->payload);
        producer_done++;
      }
    }

    // Drain remaining
    while (true)
    {
      Task* t = get_work(0, num_threads, threads);
      if (!t)
        break;
      producer_sum += do_work(t->payload);
      producer_done++;
    }

    g_stop.store(true, std::memory_order_release);
    for (auto& th : workers)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    *tasks_completed =
      producer_done + g_tasks_done.load(std::memory_order_relaxed);

    for (int i = 0; i < num_threads; i++)
      delete threads[i].queue;

    return ms;
  }
} // namespace bench_vr_flat

// ============================================================
// Scenario 2: All-to-all (every core is both producer and thief)
//
// Each core gets N/K tasks. Processes its own, steals when empty.
// Models actor/cown workloads where work arrives on many cores.
// ============================================================

namespace bench_cl_scatter
{
  static double run(
    size_t num_tasks, int num_threads, size_t* tasks_completed)
  {
    std::vector<Task> pool(num_tasks);
    for (size_t i = 0; i < num_tasks; i++)
      pool[i].payload = i + 1;

    std::vector<chase_lev::Deque<Task>*> deques(num_threads);
    for (int i = 0; i < num_threads; i++)
      deques[i] = new chase_lev::Deque<Task>();

    // Distribute tasks evenly across all deques
    size_t per_thread = num_tasks / num_threads;
    for (int i = 0; i < num_threads; i++)
    {
      size_t start = i * per_thread;
      size_t end = (i == num_threads - 1) ? num_tasks : start + per_thread;
      for (size_t j = start; j < end; j++)
        deques[i]->push(&pool[j]);
    }

    std::atomic<size_t> total_done{0};
    std::atomic<bool> stop{false};

    auto worker = [&](int id) {
      size_t local_done = 0;
      uint64_t local_sum = 0;

      while (!stop.load(std::memory_order_relaxed))
      {
        Task* t = deques[id]->pop();
        if (!t)
        {
          // Steal
          bool found = false;
          for (int i = 0; i < num_threads; i++)
          {
            int victim = (id + 1 + i) % num_threads;
            if (victim == id)
              continue;
            t = deques[victim]->steal();
            if (t)
            {
              found = true;
              break;
            }
          }
          if (!found)
            break; // All done
        }
        if (t)
        {
          local_sum += do_work(t->payload);
          local_done++;
        }
      }
      total_done.fetch_add(local_done, std::memory_order_relaxed);
    };

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < num_threads; i++)
      threads.emplace_back(worker, i);
    worker(0);

    stop.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    *tasks_completed = total_done.load(std::memory_order_relaxed);

    for (int i = 0; i < num_threads; i++)
      delete deques[i];
    return ms;
  }
} // namespace bench_cl_scatter

namespace bench_vr_scatter
{
  using ThreadState = bench_vr_flat::ThreadState;

  static double run(
    size_t num_tasks, int num_threads, size_t* tasks_completed)
  {
    std::vector<Task> pool(num_tasks);
    for (size_t i = 0; i < num_tasks; i++)
      pool[i].payload = i + 1;

    std::vector<ThreadState> states(num_threads);
    for (int i = 0; i < num_threads; i++)
      states[i].queue = new verona_wsq::WorkStealingQueue<4>();

    // Distribute tasks evenly
    size_t per_thread = num_tasks / num_threads;
    for (int i = 0; i < num_threads; i++)
    {
      size_t start = i * per_thread;
      size_t end = (i == num_threads - 1) ? num_tasks : start + per_thread;
      for (size_t j = start; j < end; j++)
        states[i].queue->enqueue(&pool[j].work);
    }

    std::atomic<size_t> total_done{0};
    std::atomic<bool> stop{false};

    auto worker = [&](int id) {
      size_t local_done = 0;
      uint64_t local_sum = 0;
      auto* q = states[id].queue;

      while (!stop.load(std::memory_order_relaxed))
      {
        verona_wsq::Work* w = q->pop_local();
        if (!w)
          w = q->refill_steal(*q);
        if (!w)
        {
          // Steal from others
          bool found = false;
          for (int i = 0; i < num_threads; i++)
          {
            int victim = (id + 1 + i) % num_threads;
            if (victim == id)
              continue;
            w = q->template refill_steal<verona_wsq::ProbeOrder::Heavy>(
              *states[victim].queue);
            if (w)
            {
              found = true;
              break;
            }
          }
          if (!found)
            break;
        }
        if (w)
        {
          Task* t = reinterpret_cast<Task*>(w);
          local_sum += do_work(t->payload);
          local_done++;
        }
      }
      total_done.fetch_add(local_done, std::memory_order_relaxed);
    };

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < num_threads; i++)
      threads.emplace_back(worker, i);
    worker(0);

    stop.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    *tasks_completed = total_done.load(std::memory_order_relaxed);

    for (int i = 0; i < num_threads; i++)
      delete states[i].queue;
    return ms;
  }
} // namespace bench_vr_scatter

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
  size_t num_tasks = 500'000;
  int batch_size = 64;
  int num_threads = 2;
  int reps = 5;

  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--tasks") && i + 1 < argc)
      num_tasks = atol(argv[++i]);
    else if (!strcmp(argv[i], "--batch") && i + 1 < argc)
      batch_size = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cores") && i + 1 < argc)
      num_threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc)
      reps = atoi(argv[++i]);
  }

  printf("=== Flat-Task Steal Benchmark (steal-all vs steal-one) ===\n");
  printf("tasks=%zu, batch=%d, cores=%d, reps=%d\n", num_tasks, batch_size, num_threads, reps);
  printf("Work per task: ~5ns (hash). Queue overhead dominates.\n\n");

  // --- Scenario 1: Producer-Consumer ---
  printf("--- Scenario 1: Producer-Consumer (1 producer, %d thieves) ---\n",
         num_threads - 1);

  double cl_best = 1e18;
  size_t cl_done = 0;
  for (int r = 0; r < reps; r++)
  {
    size_t done = 0;
    double ms = bench_cl_flat::run(num_tasks, batch_size, num_threads, &done);
    if (ms < cl_best)
    {
      cl_best = ms;
      cl_done = done;
    }
  }
  printf("Chase-Lev:  %8.2f ms  (%zu tasks, %.1f Mtask/s)\n",
         cl_best, cl_done, cl_done / cl_best / 1000.0);

  double vr_best = 1e18;
  size_t vr_done = 0;
  for (int r = 0; r < reps; r++)
  {
    size_t done = 0;
    double ms = bench_vr_flat::run(num_tasks, batch_size, num_threads, &done);
    if (ms < vr_best)
    {
      vr_best = ms;
      vr_done = done;
    }
  }
  printf("Verona WSQ: %8.2f ms  (%zu tasks, %.1f Mtask/s)\n",
         vr_best, vr_done, vr_done / vr_best / 1000.0);

  printf("Verona/CL throughput: %.2fx\n\n", (vr_done / vr_best) / (cl_done / cl_best));

  // --- Scenario 2: Scatter (all cores produce + consume) ---
  printf("--- Scenario 2: Scatter (all %d cores produce + steal) ---\n",
         num_threads);

  cl_best = 1e18;
  cl_done = 0;
  for (int r = 0; r < reps; r++)
  {
    size_t done = 0;
    double ms = bench_cl_scatter::run(num_tasks, num_threads, &done);
    if (ms < cl_best)
    {
      cl_best = ms;
      cl_done = done;
    }
  }
  printf("Chase-Lev:  %8.2f ms  (%zu tasks, %.1f Mtask/s)\n",
         cl_best, cl_done, cl_done / cl_best / 1000.0);

  vr_best = 1e18;
  vr_done = 0;
  for (int r = 0; r < reps; r++)
  {
    size_t done = 0;
    double ms = bench_vr_scatter::run(num_tasks, num_threads, &done);
    if (ms < vr_best)
    {
      vr_best = ms;
      vr_done = done;
    }
  }
  printf("Verona WSQ: %8.2f ms  (%zu tasks, %.1f Mtask/s)\n",
         vr_best, vr_done, vr_done / vr_best / 1000.0);

  printf("Verona/CL throughput: %.2fx\n", (vr_done / vr_best) / (cl_done / cl_best));

  return 0;
}
