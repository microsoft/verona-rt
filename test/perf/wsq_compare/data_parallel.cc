// Data-parallel benchmarks: Verona WSQ vs Chase-Lev
//
// Implements the key data-parallel benchmarks from Acar, Charguéraud,
// Rainey (PPoPP 2013) — scan (prefix sum), radix sort, and SpMV —
// using both WSQ backends to compare steal-all vs steal-one on
// workloads where Cilk's "big item at the top" invariant does NOT hold.
//
// These are flat task pools with many small uniform items. No tree
// structure guarantees that stolen items are large. This is where
// steal-all should dominate.
//
// Build:
//   c++ -std=c++20 -O3 -DNDEBUG -mcx16 \
//     data_parallel.cc -o data_parallel -pthread -latomic

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <thread>
#include <utility>
#include <vector>

// ============================================================
// Chase-Lev Deque
// ============================================================

namespace chase_lev
{
  template<size_t LogCapacity = 20>
  class Deque
  {
    static constexpr size_t CAPACITY = 1ULL << LogCapacity;
    static constexpr size_t MASK = CAPACITY - 1;

    alignas(64) std::atomic<int64_t> top_{0};
    alignas(64) std::atomic<int64_t> bottom_{0};
    alignas(64) void* buffer_[CAPACITY]{};

  public:
    void push(void* item)
    {
      int64_t b = bottom_.load(std::memory_order_relaxed);
      buffer_[b & MASK] = item;
      std::atomic_thread_fence(std::memory_order_release);
      bottom_.store(b + 1, std::memory_order_relaxed);
    }

    void* pop()
    {
      int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
      bottom_.store(b, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t t = top_.load(std::memory_order_relaxed);
      if (t <= b)
      {
        void* item = buffer_[b & MASK];
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

    void* steal()
    {
      int64_t t = top_.load(std::memory_order_acquire);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t b = bottom_.load(std::memory_order_acquire);
      if (t < b)
      {
        void* item = buffer_[t & MASK];
        if (!top_.compare_exchange_strong(
              t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
          return nullptr;
        return item;
      }
      return nullptr;
    }

    void reset()
    {
      top_.store(0, std::memory_order_relaxed);
      bottom_.store(0, std::memory_order_relaxed);
    }
  };
} // namespace chase_lev

// ============================================================
// Verona-style 3-stack WSQ
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
// Generic parallel-for infrastructure (both backends)
// ============================================================

// A range task: process array[lo..hi)
struct RangeTask
{
  verona_wsq::Work work; // WSQ linkage (first member)
  size_t lo;
  size_t hi;
};

// Per-thread state
struct ThreadCtx
{
  chase_lev::Deque<>* cl_deque{nullptr};
  verona_wsq::WorkStealingQueue<4>* vr_queue{nullptr};
  // Task arena
  std::vector<RangeTask> pool;
  size_t pool_next{0};

  RangeTask* alloc_task()
  {
    assert(pool_next < pool.size());
    RangeTask* t = &pool[pool_next++];
    t->work.next_in_queue = nullptr;
    return t;
  }

  void reset()
  {
    pool_next = 0;
  }
};

static int g_num_threads = 2;
static size_t g_grain = 1024; // min chunk size before going sequential
static std::vector<ThreadCtx> g_ctx;
static std::atomic<bool> g_stop{false};
static std::atomic<size_t> g_tasks_done{0};
static thread_local int tl_id = 0;

// Function pointer for the actual work
typedef void (*work_fn)(size_t lo, size_t hi, void* arg);
static work_fn g_work_fn = nullptr;
static void* g_work_arg = nullptr;

// ---- Chase-Lev parallel-for ----

namespace par_cl
{
  static void process(RangeTask* t)
  {
    size_t lo = t->lo, hi = t->hi;
    if (hi - lo <= g_grain)
    {
      g_work_fn(lo, hi, g_work_arg);
      g_tasks_done.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // Split into two halves
    size_t mid = lo + (hi - lo) / 2;
    auto& ctx = g_ctx[tl_id];
    RangeTask* left = ctx.alloc_task();
    left->lo = lo;
    left->hi = mid;
    ctx.cl_deque->push(left);

    // Execute right inline
    RangeTask right_task;
    right_task.lo = mid;
    right_task.hi = hi;
    process(&right_task);
  }

  static void worker(int id)
  {
    tl_id = id;
    auto* deque = g_ctx[id].cl_deque;
    while (!g_stop.load(std::memory_order_relaxed))
    {
      void* item = deque->pop();
      if (item)
      {
        process(static_cast<RangeTask*>(item));
        continue;
      }
      // Steal
      bool found = false;
      for (int i = 0; i < g_num_threads; i++)
      {
        int victim = (id + 1 + i) % g_num_threads;
        if (victim == id)
          continue;
        item = g_ctx[victim].cl_deque->steal();
        if (item)
        {
          process(static_cast<RangeTask*>(item));
          found = true;
          break;
        }
      }
      if (!found)
        break;
    }
  }

  static double run(size_t n, size_t grain, work_fn fn, void* arg)
  {
    g_grain = grain;
    g_work_fn = fn;
    g_work_arg = arg;
    g_stop.store(false);
    g_tasks_done.store(0);

    for (auto& ctx : g_ctx)
    {
      ctx.reset();
      ctx.cl_deque->reset();
    }

    // Root task
    RangeTask* root = g_ctx[0].alloc_task();
    root->lo = 0;
    root->hi = n;
    g_ctx[0].cl_deque->push(root);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < g_num_threads; i++)
      threads.emplace_back(worker, i);
    worker(0);

    g_stop.store(true);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
} // namespace par_cl

// ---- Verona WSQ parallel-for ----

namespace par_vr
{
  static RangeTask* get_work(int id)
  {
    auto* q = g_ctx[id].vr_queue;
    verona_wsq::Work* w = q->pop_local();
    if (w)
      return reinterpret_cast<RangeTask*>(w);
    w = q->refill_steal(*q);
    if (w)
      return reinterpret_cast<RangeTask*>(w);
    for (int i = 0; i < g_num_threads; i++)
    {
      int victim = (id + 1 + i) % g_num_threads;
      if (victim == id)
        continue;
      w = q->template refill_steal<verona_wsq::ProbeOrder::Heavy>(
        *g_ctx[victim].vr_queue);
      if (w)
        return reinterpret_cast<RangeTask*>(w);
    }
    return nullptr;
  }

  static void process(RangeTask* t)
  {
    size_t lo = t->lo, hi = t->hi;
    if (hi - lo <= g_grain)
    {
      g_work_fn(lo, hi, g_work_arg);
      g_tasks_done.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    size_t mid = lo + (hi - lo) / 2;
    auto& ctx = g_ctx[tl_id];
    RangeTask* left = ctx.alloc_task();
    left->lo = lo;
    left->hi = mid;
    ctx.vr_queue->enqueue(&left->work);

    RangeTask right_task;
    right_task.lo = mid;
    right_task.hi = hi;
    process(&right_task);
  }

  static void worker(int id)
  {
    tl_id = id;
    while (!g_stop.load(std::memory_order_relaxed))
    {
      RangeTask* t = get_work(id);
      if (t)
      {
        process(t);
        continue;
      }
      break;
    }
  }

  static double run(size_t n, size_t grain, work_fn fn, void* arg)
  {
    g_grain = grain;
    g_work_fn = fn;
    g_work_arg = arg;
    g_stop.store(false);
    g_tasks_done.store(0);

    for (auto& ctx : g_ctx)
      ctx.reset();

    RangeTask* root = g_ctx[0].alloc_task();
    root->lo = 0;
    root->hi = n;
    g_ctx[0].vr_queue->enqueue(&root->work);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < g_num_threads; i++)
      threads.emplace_back(worker, i);
    worker(0);

    g_stop.store(true);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
} // namespace par_vr

// ============================================================
// Benchmark 1: Parallel Scan (prefix sum)
// ============================================================

namespace bench_scan
{
  static int64_t* g_data = nullptr;
  static int64_t* g_out = nullptr;

  // Up-sweep: compute block sums
  static void upsweep_work(size_t lo, size_t hi, void*)
  {
    int64_t sum = 0;
    for (size_t i = lo; i < hi; i++)
      sum += g_data[i];
    g_out[lo] = sum; // store block sum at first element of block
  }

  // Down-sweep: prefix sum within block given a prefix
  static void downsweep_work(size_t lo, size_t hi, void*)
  {
    int64_t prefix = g_out[lo]; // prefix stored here by sequential phase
    for (size_t i = lo; i < hi; i++)
    {
      prefix += g_data[i];
      g_out[i] = prefix;
    }
  }

  struct Result
  {
    double serial_ms;
    double cl_ms;
    double vr_ms;
    int64_t check; // last element of prefix sum
  };

  static Result run(size_t n, size_t grain, int reps)
  {
    g_data = new int64_t[n];
    g_out = new int64_t[n];

    std::mt19937 rng(42);
    for (size_t i = 0; i < n; i++)
      g_data[i] = (rng() % 100) - 50;

    // Serial baseline
    double serial_best = 1e18;
    int64_t expected = 0;
    for (int r = 0; r < reps; r++)
    {
      auto t0 = std::chrono::high_resolution_clock::now();
      int64_t sum = 0;
      for (size_t i = 0; i < n; i++)
      {
        sum += g_data[i];
        g_out[i] = sum;
      }
      auto t1 = std::chrono::high_resolution_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (ms < serial_best)
        serial_best = ms;
      expected = g_out[n - 1];
    }

    // Parallel scan: up-sweep (parallel) + sequential prefix + down-sweep (parallel)
    // This is the standard two-pass parallel prefix sum

    size_t num_blocks = (n + grain - 1) / grain;
    std::vector<int64_t> block_sums(num_blocks);

    auto parallel_scan = [&](auto par_run) -> double {
      double best = 1e18;
      for (int r = 0; r < reps; r++)
      {
        memset(g_out, 0, n * sizeof(int64_t));

        auto t0 = std::chrono::high_resolution_clock::now();

        // Phase 1: parallel block sums
        par_run(n, grain, upsweep_work, nullptr);

        // Phase 2: sequential prefix of block sums
        // Collect block sums and compute prefix
        int64_t prefix = 0;
        for (size_t b = 0; b < num_blocks; b++)
        {
          size_t lo = b * grain;
          block_sums[b] = g_out[lo];
          g_out[lo] = prefix; // store incoming prefix for down-sweep
          prefix += block_sums[b];
        }

        // Phase 3: parallel down-sweep with prefixes
        par_run(n, grain, downsweep_work, nullptr);

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best)
          best = ms;
      }
      return best;
    };

    double cl_ms = parallel_scan(par_cl::run);
    double vr_ms = parallel_scan(par_vr::run);

    Result res{serial_best, cl_ms, vr_ms, expected};
    delete[] g_data;
    delete[] g_out;
    return res;
  }
} // namespace bench_scan

// ============================================================
// Benchmark 2: Parallel Map (element-wise transform)
//   — models the scatter/transform phase of radix sort
// ============================================================

namespace bench_map
{
  static double* g_src = nullptr;
  static double* g_dst = nullptr;

  // Compute-heavy element-wise transform (~50ns per element)
  static void map_work(size_t lo, size_t hi, void*)
  {
    for (size_t i = lo; i < hi; i++)
    {
      double x = g_src[i];
      // A few iterations of Newton's method for sqrt — non-trivial work
      double y = x * 0.5;
      for (int iter = 0; iter < 3; iter++)
        y = y - (y * y - x) / (2.0 * y);
      g_dst[i] = y;
    }
  }

  struct Result
  {
    double serial_ms;
    double cl_ms;
    double vr_ms;
  };

  static Result run(size_t n, size_t grain, int reps)
  {
    g_src = new double[n];
    g_dst = new double[n];

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(1.0, 1000.0);
    for (size_t i = 0; i < n; i++)
      g_src[i] = dist(rng);

    // Serial
    double serial_best = 1e18;
    for (int r = 0; r < reps; r++)
    {
      auto t0 = std::chrono::high_resolution_clock::now();
      map_work(0, n, nullptr);
      auto t1 = std::chrono::high_resolution_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (ms < serial_best)
        serial_best = ms;
    }

    // Parallel
    double cl_best = 1e18;
    for (int r = 0; r < reps; r++)
    {
      double ms = par_cl::run(n, grain, map_work, nullptr);
      if (ms < cl_best)
        cl_best = ms;
    }

    double vr_best = 1e18;
    for (int r = 0; r < reps; r++)
    {
      double ms = par_vr::run(n, grain, map_work, nullptr);
      if (ms < vr_best)
        vr_best = ms;
    }

    Result res{serial_best, cl_best, vr_best};
    delete[] g_src;
    delete[] g_dst;
    return res;
  }
} // namespace bench_map

// ============================================================
// Benchmark 3: Sparse Matrix-Vector Multiply (SpMV)
// ============================================================

namespace bench_spmv
{
  // CSR format
  static size_t g_nrows = 0;
  static std::vector<size_t> g_row_ptr;
  static std::vector<size_t> g_col_idx;
  static std::vector<double> g_values;
  static std::vector<double> g_x; // input vector
  static double* g_y = nullptr;   // output vector

  static void spmv_work(size_t lo, size_t hi, void*)
  {
    for (size_t row = lo; row < hi; row++)
    {
      double sum = 0.0;
      size_t start = g_row_ptr[row];
      size_t end = g_row_ptr[row + 1];
      for (size_t j = start; j < end; j++)
        sum += g_values[j] * g_x[g_col_idx[j]];
      g_y[row] = sum;
    }
  }

  struct Result
  {
    double serial_ms;
    double cl_ms;
    double vr_ms;
    size_t nnz;
    double max_err;
  };

  // Generate a random sparse matrix (avg nnz_per_row nonzeros per row)
  static void gen_sparse(size_t nrows, size_t ncols, int nnz_per_row)
  {
    g_nrows = nrows;
    g_row_ptr.resize(nrows + 1);
    g_col_idx.clear();
    g_values.clear();
    g_x.resize(ncols);

    std::mt19937 rng(77);
    std::uniform_real_distribution<double> val_dist(-1.0, 1.0);
    std::uniform_int_distribution<size_t> col_dist(0, ncols - 1);
    // Vary nnz per row (Poisson-like)
    std::poisson_distribution<int> nnz_dist(nnz_per_row);

    g_row_ptr[0] = 0;
    for (size_t i = 0; i < nrows; i++)
    {
      int nnz = std::max(1, nnz_dist(rng));
      for (int j = 0; j < nnz; j++)
      {
        g_col_idx.push_back(col_dist(rng));
        g_values.push_back(val_dist(rng));
      }
      g_row_ptr[i + 1] = g_col_idx.size();
    }

    for (size_t i = 0; i < ncols; i++)
      g_x[i] = val_dist(rng);
  }

  static Result run(size_t nrows, size_t ncols, int nnz_per_row, size_t grain, int reps)
  {
    gen_sparse(nrows, ncols, nnz_per_row);
    g_y = new double[nrows];

    // Serial
    double serial_best = 1e18;
    std::vector<double> y_serial(nrows);
    for (int r = 0; r < reps; r++)
    {
      auto t0 = std::chrono::high_resolution_clock::now();
      for (size_t row = 0; row < nrows; row++)
      {
        double sum = 0.0;
        for (size_t j = g_row_ptr[row]; j < g_row_ptr[row + 1]; j++)
          sum += g_values[j] * g_x[g_col_idx[j]];
        y_serial[row] = sum;
      }
      auto t1 = std::chrono::high_resolution_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (ms < serial_best)
        serial_best = ms;
    }

    // Parallel
    double cl_best = 1e18;
    for (int r = 0; r < reps; r++)
    {
      memset(g_y, 0, nrows * sizeof(double));
      double ms = par_cl::run(nrows, grain, spmv_work, nullptr);
      if (ms < cl_best)
        cl_best = ms;
    }

    double vr_best = 1e18;
    for (int r = 0; r < reps; r++)
    {
      memset(g_y, 0, nrows * sizeof(double));
      double ms = par_vr::run(nrows, grain, spmv_work, nullptr);
      if (ms < vr_best)
        vr_best = ms;
    }

    // Check
    double max_err = 0;
    for (size_t i = 0; i < nrows; i++)
      max_err = std::max(max_err, std::abs(g_y[i] - y_serial[i]));

    Result res{serial_best, cl_best, vr_best, g_col_idx.size(), max_err};
    delete[] g_y;
    return res;
  }
} // namespace bench_spmv

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
  int num_threads = 2;
  int reps = 5;
  size_t scan_n = 10'000'000;
  size_t radix_n = 2'000'000;
  size_t spmv_rows = 200'000;
  size_t grain = 4096;

  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--cores") && i + 1 < argc)
      num_threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc)
      reps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--scan_n") && i + 1 < argc)
      scan_n = atol(argv[++i]);
    else if (!strcmp(argv[i], "--radix_n") && i + 1 < argc)
      radix_n = atol(argv[++i]);
    else if (!strcmp(argv[i], "--spmv_rows") && i + 1 < argc)
      spmv_rows = atol(argv[++i]);
    else if (!strcmp(argv[i], "--grain") && i + 1 < argc)
      grain = atol(argv[++i]);
  }

  g_num_threads = num_threads;

  printf("=== Data-Parallel Benchmarks (PASL-style) ===\n");
  printf("cores=%d, grain=%zu, reps=%d\n\n", num_threads, grain, reps);

  // Initialize per-thread contexts
  size_t max_tasks = 100'000; // enough for recursive splitting
  g_ctx.resize(num_threads);
  for (int i = 0; i < num_threads; i++)
  {
    g_ctx[i].cl_deque = new chase_lev::Deque<>();
    g_ctx[i].vr_queue = new verona_wsq::WorkStealingQueue<4>();
    g_ctx[i].pool.resize(max_tasks);
  }

  // --- Benchmark 1: Prefix Sum ---
  printf("--- Scan (prefix sum), n=%zu ---\n", scan_n);
  auto scan_res = bench_scan::run(scan_n, grain, reps);
  printf("  Serial:     %8.2f ms\n", scan_res.serial_ms);
  printf("  Chase-Lev:  %8.2f ms  (%.2fx)\n", scan_res.cl_ms, scan_res.serial_ms / scan_res.cl_ms);
  printf("  Verona WSQ: %8.2f ms  (%.2fx)\n", scan_res.vr_ms, scan_res.serial_ms / scan_res.vr_ms);
  printf("  VR/CL: %.3fx\n\n", scan_res.cl_ms / scan_res.vr_ms);

  // --- Benchmark 2: Parallel Map ---
  printf("--- Parallel Map (element-wise sqrt), n=%zu ---\n", radix_n);
  auto map_res = bench_map::run(radix_n, grain, reps);
  printf("  Serial:     %8.2f ms\n", map_res.serial_ms);
  printf("  Chase-Lev:  %8.2f ms  (%.2fx)\n", map_res.cl_ms, map_res.serial_ms / map_res.cl_ms);
  printf("  Verona WSQ: %8.2f ms  (%.2fx)\n", map_res.vr_ms, map_res.serial_ms / map_res.vr_ms);
  printf("  VR/CL: %.3fx\n\n", map_res.cl_ms / map_res.vr_ms);

  // --- Benchmark 3: SpMV ---
  printf("--- SpMV, %zu rows, ~20 nnz/row ---\n", spmv_rows);
  auto spmv_res = bench_spmv::run(spmv_rows, spmv_rows, 20, grain, reps);
  printf("  nnz=%zu\n", spmv_res.nnz);
  printf("  Serial:     %8.2f ms\n", spmv_res.serial_ms);
  printf("  Chase-Lev:  %8.2f ms  (%.2fx)\n", spmv_res.cl_ms, spmv_res.serial_ms / spmv_res.cl_ms);
  printf("  Verona WSQ: %8.2f ms  (%.2fx)\n", spmv_res.vr_ms, spmv_res.serial_ms / spmv_res.vr_ms);
  printf("  VR/CL: %.3fx\n", spmv_res.cl_ms / spmv_res.vr_ms);
  if (spmv_res.max_err > 1e-10)
    printf("  WARNING: max error = %e\n", spmv_res.max_err);

  // Cleanup
  for (int i = 0; i < num_threads; i++)
  {
    delete g_ctx[i].cl_deque;
    delete g_ctx[i].vr_queue;
  }

  return 0;
}
