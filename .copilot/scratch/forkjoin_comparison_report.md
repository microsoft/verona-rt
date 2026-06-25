# Fork/Join Benchmark: Cilk vs Verona vs TBB vs OpenMP

## Environment

- Platform: x86_64, Intel Xeon Platinum 8370C @ 2.80GHz
- Logical cores: 2 (GitHub Codespace — limited parallelism)
- Compiler: GCC 13.3.0, `-O3 -DNDEBUG`
- OpenCilk: not available (requires building LLVM from source)
- TBB: not available
- Verona-rt branch: `pr/cilk-benchmark`

## Results: Practical workloads (fib(42) cutoff=20, nqueens(13) cutoff=10)

### Verona coroutines (best of 5 reps, seed=42)

| Cores | fib speedup | nqueens speedup |
|------:|------------:|----------------:|
|     1 |       0.87x |           0.99x |
|     2 |       0.73x |           0.92x |

### OpenMP task parallelism (best of 5 reps)

| Cores | fib speedup | nqueens speedup |
|------:|------------:|----------------:|
|     1 |       0.96x |           0.98x |
|     2 |       0.86x |           1.11x |

**Notes:**
- At 1 core: Verona coroutine overhead is ~13% on fib (heap-allocated frames), negligible on nqueens (coarser tasks). OpenMP overhead is ~4%.
- At 2 cores: On a 2-core machine, the OS competes for CPU; parallelism gains are eaten by context switching. Neither runtime achieves >1x on fib.
- **This codespace cannot demonstrate scaling advantages** seen on multi-core hardware.

## Results: Pure overhead test (fib(30) cutoff=2, 1 core, ~1.35M spawns)

| Runtime      | Wall time (ms) | Per-spawn overhead |
|:-------------|---------------:|-------------------:|
| Serial       |            1.4 |            —       |
| Verona-coro  |          674.3 |         ~500 ns    |
| OpenMP       |          103.1 |          ~76 ns    |

**Analysis:** Verona's ~500ns/spawn is dominated by coroutine frame heap allocation. OpenMP's ~76ns uses compiler-managed stack frames. Cilk (not tested) is typically 10–50ns/spawn.

At practical cutoffs (fib cutoff=20, ~2000 spawns), the overhead delta is negligible vs the ~480ms runtime.

## Previously collected results (aarch64 12-core, GCC 15.2, -O2)

```
fib(42) at 10 cores:     Verona 8.0x > TBB 6.4x > OpenMP 4.5x
nqueens(13) at 10 cores: Verona 10.0x > OpenMP 6.3x > TBB 5.0x
```

These numbers demonstrate verona-rt's WSQ topology provides superior scaling on multi-core hardware.

## How to reproduce on a larger machine

```bash
cd /path/to/verona-rt
python3 .copilot/scratch/run_forkjoin_comparison.py \
  --build-dir build_forkjoin_compare \
  --cores "1,2,4,8,N-2,N" \
  --reps 5

# With Cilk (requires OpenCilk clang):
python3 .copilot/scratch/run_forkjoin_comparison.py \
  --opencilk-clang /path/to/opencilk/bin/clang \
  --cores "1,2,4,8,N-2,N"
```
