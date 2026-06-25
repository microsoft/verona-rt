// OpenCilk fork/join microbenchmarks: fib and nqueens.

#define _POSIX_C_SOURCE 199309L

#include <cilk/cilk.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fib_cutoff = 20;
static int g_nq_cutoff = 10;

static int64_t fib_serial(int n)
{
  if (n < 2)
    return n;
  return fib_serial(n - 1) + fib_serial(n - 2);
}

static int nqueens_serial(
  int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2)
{
  if (row == n)
    return 1;

  int count = 0;
  uint32_t avail = ((1u << n) - 1) & ~(cols | diag1 | diag2);
  while (avail)
  {
    uint32_t bit = avail & -avail;
    avail ^= bit;
    count += nqueens_serial(
      n, row + 1, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1);
  }
  return count;
}

static int64_t fib_cilk(int n)
{
  if (n < g_fib_cutoff)
    return fib_serial(n);

  int64_t x = cilk_spawn fib_cilk(n - 1);
  int64_t y = fib_cilk(n - 2);
  cilk_sync;
  return x + y;
}

static int nqueens_cilk(
  int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2)
{
  if (row == n)
    return 1;
  if (n - row <= g_nq_cutoff)
    return nqueens_serial(n, row, cols, diag1, diag2);

  uint32_t avail = ((1u << n) - 1) & ~(cols | diag1 | diag2);
  int results[16] = {0};
  int idx = 0;
  while (avail)
  {
    uint32_t bit = avail & -avail;
    avail ^= bit;
    int i = idx++;
    results[i] = cilk_spawn nqueens_cilk(
      n, row + 1, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1);
  }
  cilk_sync;

  int total = 0;
  for (int i = 0; i < idx; i++)
    total += results[i];
  return total;
}

typedef int64_t (*fib_fn)(int);
typedef int (*nq_fn)(int, int, uint32_t, uint32_t, uint32_t);

static double elapsed_ms(struct timespec start, struct timespec end)
{
  return (end.tv_sec - start.tv_sec) * 1e3 +
    (end.tv_nsec - start.tv_nsec) * 1e-6;
}

static double time_best_fib(int reps, fib_fn fn, int n, int64_t* result)
{
  double best = 1e18;
  for (int r = 0; r < reps; r++)
  {
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    *result = fn(n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = elapsed_ms(t0, t1);
    if (ms < best)
      best = ms;
  }
  return best;
}

static double time_best_nq(int reps, nq_fn fn, int n, int* result)
{
  double best = 1e18;
  for (int r = 0; r < reps; r++)
  {
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    *result = fn(n, 0, 0, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = elapsed_ms(t0, t1);
    if (ms < best)
      best = ms;
  }
  return best;
}

int main(int argc, char** argv)
{
  int fib_n = 42;
  int nq_n = 13;
  int reps = 5;

  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--fib_n") && i + 1 < argc)
      fib_n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nq_n") && i + 1 < argc)
      nq_n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fib_cutoff") && i + 1 < argc)
      g_fib_cutoff = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nq_cutoff") && i + 1 < argc)
      g_nq_cutoff = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc)
      reps = atoi(argv[++i]);
  }

  printf(
    "fib(%d) cutoff=%d, nqueens(%d) cutoff=%d, reps=%d\n\n",
    fib_n,
    g_fib_cutoff,
    nq_n,
    g_nq_cutoff,
    reps);

  int64_t fib_expected = 0;
  double fib_serial_ms = time_best_fib(reps, fib_serial, fib_n, &fib_expected);
  printf(
    "fib(%d) serial:      %8.1f ms  (result=%ld)\n",
    fib_n,
    fib_serial_ms,
    fib_expected);

  int nq_expected = 0;
  double nq_serial_ms = time_best_nq(reps, nqueens_serial, nq_n, &nq_expected);
  printf(
    "nqueens(%d) serial:  %8.1f ms  (result=%d)\n\n",
    nq_n,
    nq_serial_ms,
    nq_expected);

  int64_t fib_result = 0;
  double fib_cilk_ms = time_best_fib(reps, fib_cilk, fib_n, &fib_result);
  printf(
    "fib(%d) Cilk:        %8.1f ms  (result=%ld, speedup=%.2fx)\n",
    fib_n,
    fib_cilk_ms,
    fib_result,
    fib_serial_ms / fib_cilk_ms);

  int nq_result = 0;
  double nq_cilk_ms = time_best_nq(reps, nqueens_cilk, nq_n, &nq_result);
  printf(
    "nqueens(%d) Cilk:    %8.1f ms  (result=%d, speedup=%.2fx)\n\n",
    nq_n,
    nq_cilk_ms,
    nq_result,
    nq_serial_ms / nq_cilk_ms);

  int ok = 1;
  if (fib_result != fib_expected)
  {
    printf("ERROR: fib mismatch: serial=%ld cilk=%ld\n", fib_expected, fib_result);
    ok = 0;
  }
  if (nq_result != nq_expected)
  {
    printf("ERROR: nqueens mismatch: serial=%d cilk=%d\n", nq_expected, nq_result);
    ok = 0;
  }
  if (ok)
    printf("All results correct.\n");

  return ok ? 0 : 1;
}