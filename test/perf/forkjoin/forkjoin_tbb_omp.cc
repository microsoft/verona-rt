// TBB + OpenMP fork/join benchmarks for comparison
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef USE_TBB
#include <tbb/task_group.h>
#include <tbb/global_control.h>
#endif

using Clock = std::chrono::high_resolution_clock;

static int g_fib_cutoff = 20;
static int g_nq_cutoff = 10;

// --- Serial ---
int64_t fib_serial(int n) {
  if (n < 2) return n;
  return fib_serial(n-1) + fib_serial(n-2);
}

int nqueens_serial(int n, int row, uint32_t cols, uint32_t d1, uint32_t d2) {
  if (row == n) return 1;
  int count = 0;
  uint32_t avail = ((1u<<n)-1) & ~(cols|d1|d2);
  while (avail) {
    uint32_t bit = avail & (-avail); avail ^= bit;
    count += nqueens_serial(n, row+1, cols|bit, (d1|bit)<<1, (d2|bit)>>1);
  }
  return count;
}

#ifdef USE_TBB
// --- TBB ---
int64_t fib_tbb(int n) {
  if (n < g_fib_cutoff) return fib_serial(n);
  int64_t x, y;
  tbb::task_group g;
  g.run([&]{ x = fib_tbb(n-1); });
  y = fib_tbb(n-2);
  g.wait();
  return x + y;
}

int nqueens_tbb(int n, int row, uint32_t cols, uint32_t d1, uint32_t d2) {
  if (row == n) return 1;
  if (n - row <= g_nq_cutoff) return nqueens_serial(n, row, cols, d1, d2);
  uint32_t avail = ((1u<<n)-1) & ~(cols|d1|d2);
  int results[16] = {};
  int idx = 0;
  tbb::task_group g;
  while (avail) {
    uint32_t bit = avail & (-avail); avail ^= bit;
    int i = idx++;
    g.run([&,n,row,cols,d1,d2,bit,i]{
      results[i] = nqueens_tbb(n, row+1, cols|bit, (d1|bit)<<1, (d2|bit)>>1);
    });
  }
  g.wait();
  int total = 0;
  for (int i = 0; i < idx; i++) total += results[i];
  return total;
}
#endif

// --- OpenMP ---
int64_t fib_omp(int n) {
  if (n < g_fib_cutoff) return fib_serial(n);
  int64_t x, y;
  #pragma omp task shared(x)
  x = fib_omp(n-1);
  y = fib_omp(n-2);
  #pragma omp taskwait
  return x + y;
}

int nqueens_omp(int n, int row, uint32_t cols, uint32_t d1, uint32_t d2) {
  if (row == n) return 1;
  if (n - row <= g_nq_cutoff) return nqueens_serial(n, row, cols, d1, d2);
  uint32_t avail = ((1u<<n)-1) & ~(cols|d1|d2);
  int results[16] = {};
  int idx = 0;
  while (avail) {
    uint32_t bit = avail & (-avail); avail ^= bit;
    int i = idx++;
    #pragma omp task shared(results) firstprivate(n,row,cols,d1,d2,bit,i)
    results[i] = nqueens_omp(n, row+1, cols|bit, (d1|bit)<<1, (d2|bit)>>1);
  }
  #pragma omp taskwait
  int total = 0;
  for (int i = 0; i < idx; i++) total += results[i];
  return total;
}

template<typename F>
double time_best_ms(int reps, F&& f) {
  double best = 1e18;
  for (int r = 0; r < reps; r++) {
    auto t0 = Clock::now();
    f();
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1-t0).count();
    if (ms < best) best = ms;
  }
  return best;
}

int main(int argc, char** argv) {
  int fib_n = 42, nq_n = 13, cores = 10, reps = 5;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--fib_n") && i+1<argc) fib_n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nq_n") && i+1<argc) nq_n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cores") && i+1<argc) cores = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fib_cutoff") && i+1<argc) g_fib_cutoff = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nq_cutoff") && i+1<argc) g_nq_cutoff = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i+1<argc) reps = atoi(argv[++i]);
  }

  printf("cores=%d, fib(%d) cutoff=%d, nqueens(%d) cutoff=%d, reps=%d\n\n",
    cores, fib_n, g_fib_cutoff, nq_n, g_nq_cutoff, reps);

  // Serial
  int64_t fib_exp = 0;
  double fib_s = time_best_ms(reps, [&]{ fib_exp = fib_serial(fib_n); });
  printf("fib(%d) serial:      %8.1f ms  (result=%ld)\n", fib_n, fib_s, fib_exp);
  int nq_exp = 0;
  double nq_s = time_best_ms(reps, [&]{ nq_exp = nqueens_serial(nq_n, 0,0,0,0); });
  printf("nqueens(%d) serial:  %8.1f ms  (result=%d)\n\n", nq_n, nq_s, nq_exp);

#ifdef USE_TBB
  {
    tbb::global_control gc(tbb::global_control::max_allowed_parallelism, cores);
    int64_t r1 = 0;
    double t1 = time_best_ms(reps, [&]{ r1 = fib_tbb(fib_n); });
    printf("fib(%d) TBB:         %8.1f ms  (result=%ld, speedup=%.2fx)\n", fib_n, t1, r1, fib_s/t1);
    int r2 = 0;
    double t2 = time_best_ms(reps, [&]{ r2 = nqueens_tbb(nq_n, 0,0,0,0); });
    printf("nqueens(%d) TBB:     %8.1f ms  (result=%d, speedup=%.2fx)\n\n", nq_n, t2, r2, nq_s/t2);
  }
#endif

  // OpenMP
  {
    int64_t r1 = 0;
    double t1 = time_best_ms(reps, [&]{
      #pragma omp parallel num_threads(cores)
      #pragma omp single
      r1 = fib_omp(fib_n);
    });
    printf("fib(%d) OpenMP:      %8.1f ms  (result=%ld, speedup=%.2fx)\n", fib_n, t1, r1, fib_s/t1);
    int r2 = 0;
    double t2 = time_best_ms(reps, [&]{
      #pragma omp parallel num_threads(cores)
      #pragma omp single
      r2 = nqueens_omp(nq_n, 0,0,0,0);
    });
    printf("nqueens(%d) OpenMP:  %8.1f ms  (result=%d, speedup=%.2fx)\n\n", nq_n, t2, r2, nq_s/t2);
  }

  return 0;
}
