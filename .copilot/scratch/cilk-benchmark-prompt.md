# Cilk vs Verona-RT Fork/Join Benchmark

## Goal

Produce an apples-to-apples comparison of fork/join scheduling between OpenCilk and verona-rt's work-stealing queue (WSQ), using the canonical Cilk benchmarks (fib, nqueens). There are three levels of comparison; do as many as practical.

## Context: What Already Exists

The verona-rt repo (branch `pr/perf-kv-and-phase0`) already has:

1. **`src/rt/cpp/forkjoin_coro.h`** — a C++20 coroutine-based fork/join API built on the verona-rt scheduler. It provides:
   - `Task` — coroutine return type (self-destroying at final_suspend)
   - `co_await fj::spawn(child)` — non-blocking, schedules child on WSQ
   - `co_await fj::sync()` — suspends until all children complete
   - `run_task(t)` / `signal_done()` — entry/exit for root tasks
   - Results communicated via references (Cilk-style)

2. **`.copilot/scratch/bench_forkjoin.cc`** — benchmark that measures serial, verona-coro on fib(42) and nqueens(13)

3. **`/tmp/bench_tbb_omp.cc`** — standalone TBB + OpenMP comparison benchmark

4. **Results already collected** (aarch64 12-core WSL, GCC 15.2, -O2):
   ```
   fib(42) at 10 cores:     Verona 8.0x > TBB 6.4x > OpenMP 4.5x
   nqueens(13) at 10 cores: Verona 10.0x > OpenMP 6.3x > TBB 5.0x
   ```

## Three Levels of Comparison

### Level 1: OpenCilk native vs Verona coroutines (different compilers, different runtimes)

Build OpenCilk from source and run the canonical benchmarks.

**Steps:**
1. Clone and build OpenCilk (LLVM fork):
   ```bash
   git clone --depth 1 https://github.com/OpenCilk/opencilk-project.git /tmp/opencilk-src
   cd /tmp/opencilk-src
   mkdir build && cd build
   cmake -G Ninja ../llvm \
     -DCMAKE_BUILD_TYPE=Release \
     -DLLVM_ENABLE_PROJECTS="clang" \
     -DLLVM_ENABLE_RUNTIMES="cheetah;compiler-rt" \
     -DLLVM_TARGETS_TO_BUILD="AArch64" \
     -DLLVM_PARALLEL_LINK_JOBS=2
   ninja clang    # ~30-60 min depending on machine
   ```
   If RAM is tight (<16GB), use `-DLLVM_PARALLEL_LINK_JOBS=1` and `ninja -j4`.

2. Write `cilk_fib.c` and `cilk_nqueens.c` using `cilk_spawn`/`cilk_sync`:
   ```c
   // cilk_fib.c
   #include <cilk/cilk.h>
   #include <stdio.h>
   #include <stdlib.h>
   #include <time.h>
   
   long fib_serial(long n) { return n < 2 ? n : fib_serial(n-1) + fib_serial(n-2); }
   
   long fib(long n) {
     if (n < 20) return fib_serial(n);  // cutoff
     long x, y;
     x = cilk_spawn fib(n-1);
     y = fib(n-2);
     cilk_sync;
     return x + y;
   }
   
   int main(int argc, char** argv) {
     long n = argc > 1 ? atol(argv[1]) : 42;
     int reps = argc > 2 ? atoi(argv[2]) : 5;
     double best = 1e18;
     long result;
     for (int r = 0; r < reps; r++) {
       struct timespec t0, t1;
       clock_gettime(CLOCK_MONOTONIC, &t0);
       result = fib(n);
       clock_gettime(CLOCK_MONOTONIC, &t1);
       double ms = (t1.tv_sec - t0.tv_sec)*1e3 + (t1.tv_nsec - t0.tv_nsec)*1e-6;
       if (ms < best) best = ms;
     }
     printf("fib(%ld) = %ld  best=%.1f ms\n", n, result, best);
   }
   ```
   Compile: `/path/to/opencilk/bin/clang -fopencilk -O2 cilk_fib.c -o cilk_fib`
   Run: `CILK_NWORKERS=10 ./cilk_fib 42 5`

3. Run the same benchmarks with verona-rt coroutines (use `bench_forkjoin.cc`):
   ```bash
   cd /path/to/verona-rt
   # Build verona-rt first:
   mkdir build_bench && cd build_bench
   cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
   ninja
   # Then compile the benchmark:
   c++ -std=c++20 -O2 -DNDEBUG \
     $(pkg-config-style flags from build) \
     .copilot/scratch/bench_forkjoin.cc -o bench_forkjoin -latomic -lpthread
   ./bench_forkjoin --seed 1 --cores 10 --fib_n 42 --nq_n 13 --fib_cutoff 20 --nq_cutoff 10 --reps 5
   ```

4. Also run TBB and OpenMP for context (use `/tmp/bench_tbb_omp.cc`).

5. Sweep cores: 1, 2, 4, 8, N-2, N (where N = nproc).

**What this measures:** End-to-end comparison including compiler optimizations. Cilk has an unfair advantage here because `cilk_spawn` avoids heap-allocating the continuation frame (uses a "cactus stack" with compiler support), while our coroutines heap-allocate every frame. This is the "how far behind are we?" measurement.

### Level 2: Same benchmark, same compiler, different runtimes (fairer)

Use the same compiler (GCC or the OpenCilk clang) for both. Compile the coroutine version with the same compiler that compiled Cilk.

```bash
# Compile verona coroutine benchmark with OpenCilk's clang (same codegen)
/path/to/opencilk/bin/clang++ -std=c++20 -O2 -DNDEBUG \
  -I verona-rt/src/rt/. -I verona-rt/build/_deps/snmalloc-src/src -I verona-rt/test \
  <snmalloc defines> \
  bench_forkjoin.cc -o bench_forkjoin_clang -latomic -lpthread
```

This isolates: Cilk's cactus-stack + THE deque vs. coroutine heap frames + verona WSQ.

### Level 3: Build a Cilk-compatible runtime on verona-rt's scheduler (true apples-to-apples)

This is the most informative but hardest. Rather than plugging verona's deque into Cilk's steal loop, we build a **complete Cilk-compatible fork/join runtime using verona-rt's scheduling topology**. The whole steal protocol should come from verona-rt's design, not Cilk's.

**Key architectural difference:**
- **Cilk's THE protocol:** Each worker has a private deque. Thieves randomly pick a victim and steal one item from the TOP (oldest) of that victim's deque. The steal is 1:1 (one thief steals one continuation from one victim).
- **Verona-rt's WSQ design:** A carefully balanced two-level structure. Per-core LIFO queue for the fast path (local push/pop with no contention). When a core has no local work, it steals from other cores. The specific balance of steal-all vs steal-one, the overflow policy, and the interaction with scheduler sleep/wake is a tuned design — not a generic queue. See `src/rt/sched/workstealingqueue.h` and `src/rt/sched/mpmcq.h` for the actual implementation.

The question is: **does verona-rt's work-stealing topology outperform Cilk's single-level random-victim stealing for fork/join workloads?**

**Approach:**
1. We already have the answer via `forkjoin_coro.h` — it IS a Cilk-compatible runtime on verona-rt's scheduler. The coroutine overhead is the confound.
2. To remove the coroutine overhead confound, build a **closure-based fork/join** runtime (no coroutines, no heap-allocated frames) that still uses verona-rt's WSQ. This matches what Cilk does at the C level: function pointers + stack frames.
3. The closure-based API would look like:
   ```cpp
   // Spawn: push work to WSQ (like cilk_spawn)
   void fj_spawn(void (*fn)(void*), void* arg);
   // Sync: wait for all children (like cilk_sync)
   void fj_sync();
   ```
   Internally, `fj_spawn` creates a `Work` item and calls `Scheduler::schedule(w, ...)`, and `fj_sync` suspends the current fiber/continuation until children complete.

4. The challenge: without coroutines or compiler support, "suspending" at sync requires either:
   - **Blocking** (spin-wait — wastes a thread, limits scalability), or
   - **Continuation-passing** (manual CPS — ugly but correct, what Cilk's compiler does), or
   - **Stackful fibers** (ucontext/Boost.Context — gives "free" suspend but has stack allocation cost)

5. **Recommended approach for a fair comparison:** Use the coroutine version (Level 2) as the primary datapoint, and note that the coroutine frame allocation (~100-200 bytes heap) is the price we pay vs Cilk's compiler-managed cactus stack. The WSQ topology comparison is already embedded in those numbers.

**What the comparison isolates:**
- Level 1 (OpenCilk vs verona-coro): measures EVERYTHING different (compiler, stack mgmt, deque, steal protocol)
- Level 2 (same compiler): isolates runtime (frame allocation + scheduling) vs Cilk runtime
- Level 3 thought experiment: if we could eliminate coroutine frame cost, our WSQ-based scheduler would be approximately:
  - `measured_verona_time - (num_spawns × coroutine_frame_cost)` 
  - We measured ~220ns/spawn in the pure-overhead test. Cilk reports ~10-20ns/spawn. The difference (~200ns) is almost entirely coroutine frame alloc/dealloc.
  - At cutoff=20, fib(42) spawns ~2000 tasks → overhead delta ≈ 0.4ms (negligible vs the 50ms runtime)
  - So at practical cutoffs, the WSQ topology is already being fairly tested.

**Alternative Level 3 (if time permits):** Build OpenCilk's cheetah runtime with its steal loop replaced by verona-rt's work-stealing design. This means:
1. Clone cheetah: `git clone https://github.com/OpenCilk/cheetah.git`
2. Replace the random-victim steal-one protocol with verona-rt's stealing topology (study `src/rt/sched/workstealingqueue.h` and `src/rt/sched/mpmcq.h` to understand the specific balance of operations)
3. Keep Cilk's cactus stack and continuation-stealing — just change HOW stolen work is located and redistributed
4. This directly answers: "Is verona's work-stealing design better than Cilk's random-victim steal-one?"

This is the hardest path but most scientifically interesting. If attempted, keep Cilk's stack management untouched and ONLY replace the work-discovery and redistribution mechanism.

## Expected Outputs

A table like:

```
====================================================================
  FORK/JOIN BENCHMARK: Cilk vs Verona vs TBB vs OpenMP
  Platform: <arch> <cores>-core, <compiler>, -O2
  fib(42) cutoff=20, nqueens(13) cutoff=10, best of 5 reps
====================================================================

fib(42) — speedup over serial
Cores   Cilk    Verona-coro   TBB     OpenMP
  1     1.00x   0.97x         0.99x   1.01x
  2     ...     ...           ...     ...
  4     ...     ...           ...     ...
  8     ...     ...           ...     ...
 10     ...     ...           ...     ...

nqueens(13) — speedup over serial
Cores   Cilk    Verona-coro   TBB     OpenMP
  ...
```

## Key Differences to Look For

1. **1-core overhead**: Cilk should be ~0% (no frame allocation). Verona coroutines showed ~3% on fib. TBB ~1%.
2. **Scaling slope**: Cilk's continuation-stealing means it can execute the "continuation" (parent) immediately and put the child on the deque. Our coroutines put the child on the deque and continue the parent — same work-first policy but with an extra heap allocation per spawn.
3. **Pure overhead test**: `fib(30) cutoff=2` (no serial work, ~1.7M spawns). This measures raw spawn/sync cost. We measured ~220ns/spawn for verona coroutines. Cilk should be ~10-50ns/spawn.
4. **Practical workloads**: `nqueens(13) cutoff=10` — enough work per task that spawn overhead is amortized. All systems should converge here.

## Build Notes

- **OpenCilk build requires:** cmake, ninja, ~15GB disk for build, ~8GB RAM for parallel linking
- If RAM is limited, use `ninja -j4` and `-DLLVM_PARALLEL_LINK_JOBS=1`
- The build produces `bin/clang` and `lib/aarch64-unknown-linux-gnu/libopencilk.so` (or similar)
- Set `CILK_NWORKERS=N` to control thread count when running Cilk binaries
- **snmalloc flags** for compiling verona code: get them from an existing build's compile_commands.json or CMakeCache.txt — the key ones are:
  ```
  -DMALLOC_USABLE_SIZE_QUALIFIER=const -DSNMALLOC_CHEAP_CHECKS
  -DSNMALLOC_HAS_LINUX_FUTEX_H -DSNMALLOC_HAS_LINUX_RANDOM_H
  -DSNMALLOC_NO_REALLOCARR -DSNMALLOC_NO_REALLOCARRAY
  -DSNMALLOC_PLATFORM_HAS_GETENTROPY -DSNMALLOC_PTHREAD_ATFORK_WORKS
  -DSNMALLOC_USE_WAIT_ON_ADDRESS=1
  ```
  These vary by platform — run `cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release` and extract from `build/compile_commands.json`.

## What NOT to Do

- Don't run fib without a cutoff (cutoff=2) as the primary comparison — it's a pure overhead pathological case. Use it as a secondary datapoint.
- Don't compare Debug builds.
- Don't use `--cores $(nproc)` — leave 2 cores for OS. Use `$(nproc) - 2`.
- Don't use `std::async` — it creates a thread per spawn and will crash or be 100x slower.
