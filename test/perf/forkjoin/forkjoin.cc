// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
//
// Fork/join microbenchmarks: fib and nqueens.
// Measures parallel speedup of verona-rt coroutine fork/join vs serial.

#include <cpp/forkjoin_coro.h>
#include <cpp/when.h>
#include <debug/harness.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace verona::rt;
using namespace verona::rt::fj;
using namespace verona::cpp;
using Clock = std::chrono::high_resolution_clock;

static int g_fib_n = 35;
static int g_nq_n = 13;
static int g_fib_cutoff = 20;
static int g_nq_cutoff = 5;
static int g_reps = 5;

// ============================================================
// Serial baselines
// ============================================================

int64_t fib_serial(int n)
{
  if (n < 2)
    return n;
  return fib_serial(n - 1) + fib_serial(n - 2);
}

int nqueens_serial(
  int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2)
{
  if (row == n)
    return 1;
  int count = 0;
  uint32_t avail = ((1u << n) - 1) & ~(cols | diag1 | diag2);
  while (avail)
  {
    uint32_t bit = avail & (-avail);
    avail ^= bit;
    count += nqueens_serial(
      n, row + 1, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1);
  }
  return count;
}

// ============================================================
// Verona coroutine versions
// ============================================================

Task fib_coro(int n, int64_t& out)
{
  if (n < g_fib_cutoff)
  {
    out = fib_serial(n);
    co_return;
  }
  int64_t a, b;
  co_await fj::spawn(fib_coro(n - 1, a));
  co_await fj::spawn(fib_coro(n - 2, b));
  co_await fj::sync();
  out = a + b;
}

Task nqueens_coro(
  int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2, int& out)
{
  if (row == n)
  {
    out = 1;
    co_return;
  }
  if (n - row <= g_nq_cutoff)
  {
    out = nqueens_serial(n, row, cols, diag1, diag2);
    co_return;
  }
  uint32_t avail = ((1u << n) - 1) & ~(cols | diag1 | diag2);
  int nchildren = __builtin_popcount(avail);
  // Stack array — max N children (N <= 16 for practical board sizes).
  int results[16] = {};
  int idx = 0;
  while (avail)
  {
    uint32_t bit = avail & (-avail);
    avail ^= bit;
    co_await fj::spawn(nqueens_coro(
      n,
      row + 1,
      cols | bit,
      (diag1 | bit) << 1,
      (diag2 | bit) >> 1,
      results[idx]));
    idx++;
  }
  co_await fj::sync();
  int total = 0;
  for (int i = 0; i < nchildren; i++)
    total += results[i];
  out = total;
}

// Root benchmark coroutines
Task bench_fib_coro(double& best_ms, int64_t& result)
{
  best_ms = 1e18;
  for (int r = 0; r < g_reps; r++)
  {
    auto t0 = Clock::now();
    co_await fj::spawn(fib_coro(g_fib_n, result));
    co_await fj::sync();
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best_ms)
      best_ms = ms;
  }
  signal_done();
}

Task bench_nqueens_coro(double& best_ms, int& result)
{
  best_ms = 1e18;
  for (int r = 0; r < g_reps; r++)
  {
    auto t0 = Clock::now();
    co_await fj::spawn(nqueens_coro(g_nq_n, 0, 0, 0, 0, result));
    co_await fj::sync();
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best_ms)
      best_ms = ms;
  }
  signal_done();
}

// ============================================================
// Timing helper
// ============================================================

template<typename F>
double time_best_ms(int reps, F&& f)
{
  double best = 1e18;
  for (int r = 0; r < reps; r++)
  {
    auto t0 = Clock::now();
    f();
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best)
      best = ms;
  }
  return best;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  for (int i = 1; i < argc; i++)
  {
    if (std::string(argv[i]) == "--fib_n" && i + 1 < argc)
      g_fib_n = std::atoi(argv[++i]);
    else if (std::string(argv[i]) == "--nq_n" && i + 1 < argc)
      g_nq_n = std::atoi(argv[++i]);
    else if (std::string(argv[i]) == "--fib_cutoff" && i + 1 < argc)
      g_fib_cutoff = std::atoi(argv[++i]);
    else if (std::string(argv[i]) == "--nq_cutoff" && i + 1 < argc)
      g_nq_cutoff = std::atoi(argv[++i]);
    else if (std::string(argv[i]) == "--reps" && i + 1 < argc)
      g_reps = std::atoi(argv[++i]);
  }

  printf("=== Fork/Join Benchmark ===\n");
  printf(
    "fib(%d) cutoff=%d, nqueens(%d) cutoff=%d, cores=%ld, reps=%d\n\n",
    g_fib_n,
    g_fib_cutoff,
    g_nq_n,
    g_nq_cutoff,
    (long)harness.cores,
    g_reps);

  // --- Serial baselines ---
  int64_t fib_expected = 0;
  double fib_serial_ms =
    time_best_ms(g_reps, [&] { fib_expected = fib_serial(g_fib_n); });
  printf(
    "fib(%d) serial:      %8.1f ms  (result=%ld)\n",
    g_fib_n,
    fib_serial_ms,
    fib_expected);

  int nq_expected = 0;
  double nq_serial_ms = time_best_ms(
    g_reps, [&] { nq_expected = nqueens_serial(g_nq_n, 0, 0, 0, 0); });
  printf(
    "nqueens(%d) serial:  %8.1f ms  (result=%d)\n\n",
    g_nq_n,
    nq_serial_ms,
    nq_expected);

  // --- Verona coroutine fork/join ---
  double coro_fib_ms = 0;
  int64_t coro_fib_result = 0;
  {
    auto* ms_ptr = &coro_fib_ms;
    auto* res_ptr = &coro_fib_result;
    harness.run([ms_ptr, res_ptr]() {
      when() << [ms_ptr, res_ptr]() {
        auto t = bench_fib_coro(*ms_ptr, *res_ptr);
        run_task(t);
      };
    });
  }
  printf(
    "fib(%d) verona-coro: %8.1f ms  (result=%ld, speedup=%.2fx)\n",
    g_fib_n,
    coro_fib_ms,
    coro_fib_result,
    fib_serial_ms / coro_fib_ms);

  double coro_nq_ms = 0;
  int coro_nq_result = 0;
  {
    auto* ms_ptr = &coro_nq_ms;
    auto* res_ptr = &coro_nq_result;
    harness.run([ms_ptr, res_ptr]() {
      when() << [ms_ptr, res_ptr]() {
        auto t = bench_nqueens_coro(*ms_ptr, *res_ptr);
        run_task(t);
      };
    });
  }
  printf(
    "nqueens(%d) v-coro:  %8.1f ms  (result=%d, speedup=%.2fx)\n\n",
    g_nq_n,
    coro_nq_ms,
    coro_nq_result,
    nq_serial_ms / coro_nq_ms);

  // Verify correctness
  bool ok = true;
  if (coro_fib_result != fib_expected)
  {
    printf(
      "ERROR: fib mismatch: serial=%ld coro=%ld\n",
      fib_expected,
      coro_fib_result);
    ok = false;
  }
  if (coro_nq_result != nq_expected)
  {
    printf(
      "ERROR: nqueens mismatch: serial=%d coro=%d\n",
      nq_expected,
      coro_nq_result);
    ok = false;
  }
  if (ok)
    printf("All results correct.\n");

  return 0;
}
