// THE vs D[4] WSQ: Cilk-style fork/join topology comparison
//
// Correctly implements both deque protocols with identical execution
// model (work-first, self-count join counter, pre-allocated frames).
//
// THE protocol (Cilk's actual Dekker-based deque):
//   Push: *tail++ = frame; tail.store(t, release)  [plain store on x86]
//   Pop:  --tail; tail.store(t, seq_cst); h = exc.load(seq_cst)
//         if h > t → stolen, else return *t
//   Steal: increment head via CAS (steal-one)
//
// D[4] WSQ (Verona topology):
//   Push: CAS-push to D[dequeue_index--] (release CAS)
//   Pop:  exchange D[idx] to nullptr (XCHG on x86), split head, store rest
//   Steal: exchange D[k] to nullptr (steal-all from one bucket)
//
// On x86, both pop paths emit XCHG (implicit LOCK). Push differs:
// THE uses a plain MOV (release store), D[4] uses LOCK CMPXCHG (CAS).
// On AArch64, THE pop needs DMB+STLR (seq_cst), D[4] pop can use
// LDXR/STXR (relaxed) — a genuine cost difference.
//
// Build:
//   c++ -std=c++20 -O3 -DNDEBUG -mcx16 cilk_wsq.cc -o cilk_wsq -pthread -latomic
//
// Run:
//   ./cilk_wsq --fib_n 35 --cutoff 20 --cores 2 --reps 30
//
// Report: prints median, min, IQR over --reps runs for each backend.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <utility>
#include <vector>

// ============================================================
// Common frame structure
// ============================================================

struct Frame
{
  // Intrusive link for D[4] (unused by THE)
  Frame* next{nullptr};

  int n;
  int64_t result{0};
  std::atomic<int> join_counter{0};
  Frame* parent{nullptr};
  int child_index{0};
  int64_t child_results[2]{};
};

// ============================================================
// Steal statistics
// ============================================================

struct StealStats
{
  std::vector<int64_t> stolen_subtree_sizes; // subtree size of each stolen item
  size_t steal_attempts{0};
  size_t steal_successes{0};
  size_t items_per_steal_total{0}; // total items obtained across all steals

  void record_steal(int64_t subtree_size)
  {
    stolen_subtree_sizes.push_back(subtree_size);
  }

  void record_attempt(bool success, size_t items = 1)
  {
    steal_attempts++;
    if (success)
    {
      steal_successes++;
      items_per_steal_total += items;
    }
  }

  void reset()
  {
    stolen_subtree_sizes.clear();
    steal_attempts = 0;
    steal_successes = 0;
    items_per_steal_total = 0;
  }

  void print(const char* label) const
  {
    printf("  %s steal stats:\n", label);
    printf("    attempts=%zu  successes=%zu  (%.0f%% hit rate)\n",
           steal_attempts, steal_successes,
           steal_attempts > 0 ? 100.0 * steal_successes / steal_attempts : 0.0);
    printf("    items_per_steal=%.1f\n",
           steal_successes > 0
             ? (double)items_per_steal_total / steal_successes
             : 0.0);

    if (stolen_subtree_sizes.empty())
    {
      printf("    no subtree size data\n");
      return;
    }

    auto sorted = stolen_subtree_sizes;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    int64_t total = 0;
    for (auto s : sorted)
      total += s;
    printf("    stolen frames: %zu\n", n);
    printf("    subtree sizes: min=%ld  median=%ld  max=%ld  mean=%.0f\n",
           sorted[0], sorted[n / 2], sorted[n - 1], (double)total / n);
    printf("    Q1=%ld  Q3=%ld\n", sorted[n / 4], sorted[3 * n / 4]);

    // Distribution buckets
    size_t tiny = 0, small = 0, med = 0, large = 0;
    for (auto s : sorted)
    {
      if (s <= 1)
        tiny++;
      else if (s <= 10)
        small++;
      else if (s <= 100)
        med++;
      else
        large++;
    }
    printf("    distribution: <=1:%zu  2-10:%zu  11-100:%zu  >100:%zu\n",
           tiny, small, med, large);
  }
};

// Per-thread steal stats (global, indexed by thread id)
static std::vector<StealStats> g_steal_stats;

// ============================================================
// Backend A: THE protocol (Cilk's Dekker-based deque)
// ============================================================

namespace the_proto
{
  static constexpr size_t LTQ_SIZE = 2048;

  // Worker with THE deque — head, tail, exc on separate cache lines
  struct alignas(128) Worker
  {
    alignas(64) Frame* ltq[LTQ_SIZE];
    alignas(64) std::atomic<int64_t> tail{0};
    alignas(64) std::atomic<int64_t> head{0};
    alignas(64) std::atomic<int64_t> exc{static_cast<int64_t>(LTQ_SIZE)};

    // Push (detach): store parent at ltq[tail], advance tail (release)
    void push(Frame* f)
    {
      int64_t t = tail.load(std::memory_order_relaxed);
      ltq[t] = f;
      tail.store(t + 1, std::memory_order_release);
    }

    // Pop (child return): Dekker protocol
    Frame* pop()
    {
      int64_t t = tail.load(std::memory_order_relaxed) - 1;
      tail.store(t, std::memory_order_seq_cst);
      int64_t h = head.load(std::memory_order_seq_cst);
      if (t > h)
      {
        return ltq[t];
      }
      if (t == h)
      {
        // Last item — race with thief on head
        if (head.compare_exchange_strong(
              h, h + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
          tail.store(t + 1, std::memory_order_relaxed);
          return ltq[t];
        }
        // Thief won
        tail.store(t + 1, std::memory_order_relaxed);
        return nullptr;
      }
      // Empty (t < h)
      tail.store(t + 1, std::memory_order_relaxed);
      return nullptr;
    }

    // Steal: CAS head forward (steal-one)
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
// Backend B: D[4] WSQ (Verona topology)
// ============================================================

namespace d4_proto
{
  static constexpr size_t N = 4;

  struct alignas(128) Worker
  {
    // Each D slot on its own cache line to avoid false sharing with thieves
    alignas(64) std::atomic<Frame*> D[N];
    size_t push_idx{N - 1};
    size_t pop_idx{0};

    Worker()
    {
      for (size_t i = 0; i < N; i++)
        D[i].store(nullptr, std::memory_order_relaxed);
    }

    // Push (detach): CAS-push to D[push_idx--]
    void push(Frame* f)
    {
      auto& slot = D[push_idx];
      push_idx = (push_idx + N - 1) % N;
      Frame* old = slot.load(std::memory_order_relaxed);
      while (true)
      {
        f->next = old;
        if (slot.compare_exchange_weak(
              old, f, std::memory_order_release, std::memory_order_relaxed))
          return;
      }
    }

    // Pop: exchange one D slot, take head, put back rest
    Frame* pop()
    {
      for (size_t attempt = 0; attempt < N; ++attempt)
      {
        size_t idx = (push_idx + 1 + attempt) % N;
        Frame* chain = D[idx].exchange(nullptr, std::memory_order_acquire);
        if (chain == nullptr)
          continue;
        Frame* result = chain;
        Frame* rest = chain->next;
        result->next = nullptr;
        if (rest)
          D[idx].store(rest, std::memory_order_release);
        return result;
      }
      return nullptr;
    }

    // Steal: exchange one D slot to nullptr (steal-all from that bucket)
    Frame* steal()
    {
      for (size_t i = 0; i < N; i++)
      {
        Frame* chain = D[i].exchange(nullptr, std::memory_order_acquire);
        if (chain)
          return chain;
      }
      return nullptr;
    }

    void reset()
    {
      for (size_t i = 0; i < N; i++)
        D[i].store(nullptr, std::memory_order_relaxed);
      push_idx = N - 1;
    }
  };
} // namespace d4_proto

// ============================================================
// Fib benchmark — templated over backend
// ============================================================

static int64_t fib_serial(int n)
{
  if (n < 2)
    return n;
  return fib_serial(n - 1) + fib_serial(n - 2);
}

template<typename Worker>
struct Bench
{
  int cutoff;
  int num_threads;
  std::vector<Worker*> workers;
  std::vector<Frame*> arenas;
  std::vector<size_t> arena_next;
  size_t arena_size{4'000'000};
  std::atomic<bool> done{false};

  Frame* alloc_frame(int id)
  {
    size_t& nx = arena_next[id];
    assert(nx < arena_size);
    Frame* f = &arenas[id][nx++];
    f->next = nullptr;
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

    // Self-count: 1(self) + 2(children) = 3
    f->join_counter.store(3, std::memory_order_relaxed);

    Frame* child_a = alloc_frame(id);
    child_a->n = f->n - 1;
    child_a->parent = f;
    child_a->child_index = 0;

    Frame* child_b = alloc_frame(id);
    child_b->n = f->n - 2;
    child_b->parent = f;
    child_b->child_index = 1;

    // Push child_a (work-first: we continue into child_b)
    workers[id]->push(child_a);

    // Execute child_b inline (depth-first)
    process(child_b, id);

    // Try to reclaim child_a
    Frame* popped = workers[id]->pop();
    if (popped == child_a)
    {
      process(child_a, id);
    }
    else
    {
      // child_a was stolen or we got something different
      if (popped)
      {
        // For D[4], popped may be a chain head — just process the one frame
        // (rest was left in the D slot by pop())
        process(popped, id);
      }
      // Spin-help until child_a completes
      while (f->join_counter.load(std::memory_order_acquire) > 1)
      {
        Frame* work = workers[id]->pop();
        if (!work)
        {
          for (int v = 0; v < num_threads; v++)
          {
            if (v == id)
              continue;
            work = workers[v]->steal();
            if (work)
            {
              // For D[4], steal returns a chain — push rest back
              Frame* rest = work->next;
              work->next = nullptr;
              while (rest)
              {
                Frame* n = rest->next;
                rest->next = nullptr;
                workers[id]->push(rest);
                rest = n;
              }
              break;
            }
          }
        }
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
        // D[4] pop already splits chain; THE returns single frame
        Frame* rest = f->next;
        f->next = nullptr;
        while (rest)
        {
          Frame* n = rest->next;
          rest->next = nullptr;
          workers[id]->push(rest);
          rest = n;
        }
        process(f, id);
        continue;
      }
      bool found = false;
      for (int v = 0; v < num_threads; v++)
      {
        if (v == id)
          continue;
        Frame* stolen = workers[v]->steal();
        if (stolen)
        {
          Frame* rest = stolen->next;
          stolen->next = nullptr;
          while (rest)
          {
            Frame* n = rest->next;
            rest->next = nullptr;
            workers[id]->push(rest);
            rest = n;
          }
          process(stolen, id);
          found = true;
          break;
        }
      }
      if (!found)
        break;
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
    for (int i = 0; i < nthreads; i++)
    {
      workers[i] = new Worker();
      arenas[i] = new Frame[arena_size]();
    }

    Frame* root = alloc_frame(0);
    root->n = fib_n;
    root->parent = nullptr;
    workers[0]->push(root);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < nthreads; i++)
      threads.emplace_back(&Bench::worker_loop, this, i);

    // Thread 0
    Frame* first = workers[0]->pop();
    if (first)
    {
      first->next = nullptr;
      process(first, 0);
    }

    done.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Stash result before cleanup
    int64_t res = root->result;
    (void)res;

    for (int i = 0; i < nthreads; i++)
    {
      delete workers[i];
      delete[] arenas[i];
    }
    workers.clear();
    arenas.clear();

    return ms;
  }
};

// ============================================================
// Unbalanced Tree Search (UTS)
//
// Each node has a deterministic but unpredictable number of children
// (derived from hashing the node's path). This breaks the depth=size
// invariant: a shallow node might be a leaf, a deep node might have
// a huge subtree. Steal-one grabs one node (might be worthless),
// steal-all grabs a batch (statistically better work transfer).
// ============================================================

struct UTSFrame
{
  Frame base; // reuse the Frame struct for queue linkage + join counter
  uint64_t state; // hash state determining children
  int depth;
  std::atomic<int64_t> subtree_count{0}; // accumulated subtree size (atomic for concurrent children)
};

namespace uts
{
  // Splitmix64 hash — deterministic, fast, good distribution
  static uint64_t hash(uint64_t x)
  {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }

  // Determine number of children: 0-8, skewed toward few
  // ~40% chance of 0 children (leaf), ~20% chance of 4+
  static int num_children_narrow(uint64_t state, int max_depth, int depth)
  {
    if (depth >= max_depth)
      return 0;
    uint64_t h = hash(state);
    unsigned val = (h >> 56); // 0-255
    if (val < 100)
      return 0; // 39% leaf
    if (val < 150)
      return 1; // 20% single child
    if (val < 190)
      return 2; // 16%
    if (val < 220)
      return 3; // 12%
    if (val < 240)
      return 4; // 8%
    if (val < 250)
      return 6; // 4%
    return 8;   // 1%
  }

  // Broad tree: high fan-out, shallower
  // ~15% leaf, ~60% chance of 4-8 children
  static int num_children_broad(uint64_t state, int max_depth, int depth)
  {
    if (depth >= max_depth)
      return 0;
    uint64_t h = hash(state);
    unsigned val = (h >> 56); // 0-255
    if (val < 40)
      return 0; // 15% leaf
    if (val < 60)
      return 1; // 8%
    if (val < 90)
      return 2; // 12%
    if (val < 130)
      return 3; // 16%
    if (val < 170)
      return 4; // 16%
    if (val < 210)
      return 6; // 16%
    return 8;   // 18%
  }

  // Function pointer for the active branching model
  static inline int (*num_children)(uint64_t, int, int) = num_children_narrow;

  // Serial UTS: count nodes in subtree
  static int64_t explore_serial(uint64_t state, int depth, int max_depth)
  {
    int nc = num_children(state, max_depth, depth);
    int64_t count = 1; // this node
    for (int i = 0; i < nc; i++)
      count += explore_serial(hash(state + i), depth + 1, max_depth);
    return count;
  }
} // namespace uts

// UTS benchmark parameterised over Worker type
template<typename Worker>
struct UTSBench
{
  int max_depth;
  int par_cutoff; // below this depth, run serial
  int num_threads;
  std::vector<Worker*> workers;
  std::vector<Frame*> arenas;
  std::vector<size_t> arena_next;
  size_t arena_size{8'000'000};
  std::atomic<bool> done{false};

  Frame* alloc_frame(int id)
  {
    size_t& nx = arena_next[id];
    assert(nx + sizeof(UTSFrame) / sizeof(Frame) <= arena_size);
    // Allocate a UTSFrame-sized chunk from the Frame arena
    Frame* f = &arenas[id][nx];
    nx += (sizeof(UTSFrame) + sizeof(Frame) - 1) / sizeof(Frame);
    f->next = nullptr;
    f->result = 0;
    f->join_counter.store(0, std::memory_order_relaxed);
    return f;
  }

  void join(Frame* f, int id)
  {
    UTSFrame* uf = reinterpret_cast<UTSFrame*>(f);
    f->result = uf->subtree_count.load(std::memory_order_relaxed);
    if (f->parent)
    {
      UTSFrame* up = reinterpret_cast<UTSFrame*>(f->parent);
      up->subtree_count.fetch_add(f->result, std::memory_order_relaxed);
      if (f->parent->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)
        join(f->parent, id);
    }
  }

  void process(Frame* f, int id)
  {
    UTSFrame* uf = reinterpret_cast<UTSFrame*>(f);
    int nc = uts::num_children(uf->state, max_depth, uf->depth);

    if (nc == 0 || uf->depth >= par_cutoff)
    {
      // Leaf or below cutoff: compute serial
      f->result = uts::explore_serial(uf->state, uf->depth, max_depth);
      if (f->parent)
      {
        UTSFrame* up = reinterpret_cast<UTSFrame*>(f->parent);
        up->subtree_count.fetch_add(f->result, std::memory_order_relaxed);
        if (f->parent->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)
          join(f->parent, id);
      }
      return;
    }

    // Fork children
    uf->subtree_count.store(1, std::memory_order_relaxed); // count this node
    f->join_counter.store(nc + 1, std::memory_order_relaxed); // self + nc children

    // Push all but last child, execute last inline (work-first)
    for (int i = 0; i < nc - 1; i++)
    {
      Frame* child = alloc_frame(id);
      UTSFrame* uc = reinterpret_cast<UTSFrame*>(child);
      uc->state = uts::hash(uf->state + i);
      uc->depth = uf->depth + 1;
      child->parent = f;
      child->result = 0;
      reinterpret_cast<UTSFrame*>(child)->subtree_count.store(0, std::memory_order_relaxed);
      workers[id]->push(child);
    }

    // Last child inline
    Frame* last_child = alloc_frame(id);
    UTSFrame* ulast = reinterpret_cast<UTSFrame*>(last_child);
    ulast->state = uts::hash(uf->state + nc - 1);
    ulast->depth = uf->depth + 1;
    last_child->parent = f;
    last_child->result = 0;
    reinterpret_cast<UTSFrame*>(last_child)->subtree_count.store(0, std::memory_order_relaxed);
    process(last_child, id);

    // Remove self-count
    if (f->join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)
      join(f, id);
    else
    {
      // Children still outstanding — help
      while (f->join_counter.load(std::memory_order_acquire) > 0)
      {
        Frame* work = workers[id]->pop();
        if (!work)
        {
          for (int v = 0; v < num_threads; v++)
          {
            if (v == id)
              continue;
            work = workers[v]->steal();
            if (work)
            {
              size_t chain_len = 0;
              Frame* cursor = work;
              while (cursor)
              {
                UTSFrame* uf = reinterpret_cast<UTSFrame*>(cursor);
                int64_t sub_size = uts::explore_serial(uf->state, uf->depth, max_depth);
                g_steal_stats[id].record_steal(sub_size);
                chain_len++;
                cursor = cursor->next;
              }
              g_steal_stats[id].record_attempt(true, chain_len);

              Frame* rest = work->next;
              work->next = nullptr;
              while (rest)
              {
                Frame* n = rest->next;
                rest->next = nullptr;
                workers[id]->push(rest);
                rest = n;
              }
              break;
            }
            else
            {
              g_steal_stats[id].record_attempt(false);
            }
          }
        }
        if (work)
          process(work, id);
      }
    }
  }

  void worker_loop(int id)
  {
    while (!done.load(std::memory_order_relaxed))
    {
      Frame* f = workers[id]->pop();
      if (f)
      {
        Frame* rest = f->next;
        f->next = nullptr;
        while (rest)
        {
          Frame* n = rest->next;
          rest->next = nullptr;
          workers[id]->push(rest);
          rest = n;
        }
        process(f, id);
        continue;
      }
      bool found = false;
      for (int v = 0; v < num_threads; v++)
      {
        if (v == id)
          continue;
        Frame* stolen = workers[v]->steal();
        if (stolen)
        {
          // Count items in stolen chain and record subtree sizes
          size_t chain_len = 0;
          Frame* cursor = stolen;
          while (cursor)
          {
            UTSFrame* uf = reinterpret_cast<UTSFrame*>(cursor);
            int64_t sub_size = uts::explore_serial(uf->state, uf->depth, max_depth);
            g_steal_stats[id].record_steal(sub_size);
            chain_len++;
            cursor = cursor->next;
          }
          g_steal_stats[id].record_attempt(true, chain_len);

          Frame* rest = stolen->next;
          stolen->next = nullptr;
          while (rest)
          {
            Frame* n = rest->next;
            rest->next = nullptr;
            workers[id]->push(rest);
            rest = n;
          }
          process(stolen, id);
          found = true;
          break;
        }
        else
        {
          g_steal_stats[id].record_attempt(false);
        }
      }
      if (!found)
        break;
    }
  }

  double run_once(uint64_t root_state, int mdepth, int pcut, int nthreads,
                  int64_t* result, bool collect_stats = false)
  {
    max_depth = mdepth;
    par_cutoff = pcut;
    num_threads = nthreads;
    done.store(false);

    g_steal_stats.resize(nthreads);
    for (auto& s : g_steal_stats)
      s.reset();

    workers.resize(nthreads);
    arenas.resize(nthreads);
    arena_next.assign(nthreads, 0);
    for (int i = 0; i < nthreads; i++)
    {
      workers[i] = new Worker();
      arenas[i] = new Frame[arena_size]();
    }

    Frame* root = alloc_frame(0);
    UTSFrame* ur = reinterpret_cast<UTSFrame*>(root);
    ur->state = root_state;
    ur->depth = 0;
    root->parent = nullptr;
    root->result = 0;
    workers[0]->push(root);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 1; i < nthreads; i++)
      threads.emplace_back(&UTSBench::worker_loop, this, i);

    // Thread 0
    Frame* first = workers[0]->pop();
    if (first)
    {
      first->next = nullptr;
      process(first, 0);
    }

    done.store(true, std::memory_order_release);
    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    UTSFrame* uroot = reinterpret_cast<UTSFrame*>(root);
    *result = uroot->subtree_count.load(std::memory_order_relaxed);

    if (collect_stats)
    {
      // Merge all threads' stats
      StealStats merged;
      for (int i = 0; i < nthreads; i++)
      {
        merged.steal_attempts += g_steal_stats[i].steal_attempts;
        merged.steal_successes += g_steal_stats[i].steal_successes;
        merged.items_per_steal_total += g_steal_stats[i].items_per_steal_total;
        for (auto s : g_steal_stats[i].stolen_subtree_sizes)
          merged.stolen_subtree_sizes.push_back(s);
      }
      merged.print("UTS");
    }

    for (int i = 0; i < nthreads; i++)
    {
      delete workers[i];
      delete[] arenas[i];
    }
    workers.clear();
    arenas.clear();

    return ms;
  }
};

// ============================================================
// Statistics helpers
// ============================================================

struct Stats
{
  double median;
  double min;
  double q1;
  double q3;
};

static Stats compute_stats(std::vector<double>& v)
{
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  Stats s;
  s.min = v[0];
  s.median = (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) / 2.0 : v[n / 2];
  s.q1 = v[n / 4];
  s.q3 = v[3 * n / 4];
  return s;
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
  int uts_depth = 18;
  int uts_cutoff = 12;
  uint64_t uts_seed = 42;
  bool uts_broad = false;

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
    else if (!strcmp(argv[i], "--uts_depth") && i + 1 < argc)
      uts_depth = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--uts_cutoff") && i + 1 < argc)
      uts_cutoff = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--uts_seed") && i + 1 < argc)
      uts_seed = atol(argv[++i]);
    else if (!strcmp(argv[i], "--uts_broad"))
      uts_broad = true;
  }

  printf("=== THE vs D[4] WSQ: Cilk-style Fork/Join ===\n");
  printf("fib(%d) cutoff=%d, cores=%d, reps=%d (interleaved)\n", fib_n, cutoff, num_threads, reps);
  printf("Reporting: median [Q1, Q3] over %d runs\n\n", reps);

  // Serial baseline
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

  // Interleaved fib runs
  Bench<the_proto::Worker> the_bench;
  Bench<d4_proto::Worker> d4_bench;
  std::vector<double> the_times;
  std::vector<double> d4_times;
  for (int r = 0; r < reps; r++)
  {
    the_times.push_back(the_bench.run_once(fib_n, cutoff, num_threads));
    d4_times.push_back(d4_bench.run_once(fib_n, cutoff, num_threads));
  }
  auto the_s = compute_stats(the_times);
  auto d4_s = compute_stats(d4_times);
  printf("THE:      median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         the_s.median, the_s.min, the_s.q1, the_s.q3);
  printf("D[4]:     median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         d4_s.median, d4_s.min, d4_s.q1, d4_s.q3);

  printf("\nFib D[4]/THE median ratio: %.3fx (>1 = D[4] faster)\n",
         the_s.median / d4_s.median);
  printf("Fib D[4]/THE min ratio:    %.3fx\n", the_s.min / d4_s.min);

  // ---- UTS (Unbalanced Tree Search) ----
  if (uts_broad)
    uts::num_children = uts::num_children_broad;
  else
    uts::num_children = uts::num_children_narrow;

  printf("\n=== UTS (Unbalanced Tree Search) ===\n");
  printf("mode=%s, depth=%d, par_cutoff=%d, seed=%lu, cores=%d, reps=%d\n",
         uts_broad ? "BROAD" : "narrow",
         uts_depth, uts_cutoff, uts_seed, num_threads, reps);

  // Serial baseline
  int64_t uts_expected = uts::explore_serial(uts_seed, 0, uts_depth);
  printf("Tree size: %ld nodes\n", uts_expected);

  std::vector<double> uts_serial_times;
  for (int r = 0; r < std::min(reps, 10); r++)
  {
    auto t0 = std::chrono::high_resolution_clock::now();
    uts::explore_serial(uts_seed, 0, uts_depth);
    auto t1 = std::chrono::high_resolution_clock::now();
    uts_serial_times.push_back(
      std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  auto uts_ser = compute_stats(uts_serial_times);
  printf("Serial:   median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         uts_ser.median, uts_ser.min, uts_ser.q1, uts_ser.q3);

  // Interleaved UTS runs
  UTSBench<the_proto::Worker> uts_the;
  UTSBench<d4_proto::Worker> uts_d4;
  std::vector<double> uts_the_times;
  std::vector<double> uts_d4_times;
  for (int r = 0; r < reps; r++)
  {
    int64_t res_the = 0, res_d4 = 0;
    bool last_run = (r == reps - 1);
    uts_the_times.push_back(
      uts_the.run_once(uts_seed, uts_depth, uts_cutoff, num_threads, &res_the, last_run));
    if (res_the != uts_expected)
      printf("  WARNING: THE UTS result=%ld expected=%ld (run %d)\n", res_the, uts_expected, r);
    uts_d4_times.push_back(
      uts_d4.run_once(uts_seed, uts_depth, uts_cutoff, num_threads, &res_d4, last_run));
    if (res_d4 != uts_expected)
      printf("  WARNING: D[4] UTS result=%ld expected=%ld (run %d)\n", res_d4, uts_expected, r);
  }
  auto uts_the_s = compute_stats(uts_the_times);
  auto uts_d4_s = compute_stats(uts_d4_times);
  printf("THE:      median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         uts_the_s.median, uts_the_s.min, uts_the_s.q1, uts_the_s.q3);
  printf("D[4]:     median=%.1fms  min=%.1fms  [%.1f, %.1f]\n",
         uts_d4_s.median, uts_d4_s.min, uts_d4_s.q1, uts_d4_s.q3);

  printf("\nUTS D[4]/THE median ratio: %.3fx (>1 = D[4] faster)\n",
         uts_the_s.median / uts_d4_s.median);
  printf("UTS D[4]/THE min ratio:    %.3fx\n", uts_the_s.min / uts_d4_s.min);

  return 0;
}
