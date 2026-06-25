#!/usr/bin/env python3

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRATCH = ROOT / ".copilot" / "scratch"
VERONA_SRC = ROOT / "test" / "perf" / "forkjoin" / "forkjoin.cc"
TBB_OMP_SRC = ROOT / "test" / "perf" / "forkjoin" / "forkjoin_tbb_omp.cc"
CILK_SRC = SCRATCH / "cilk_forkjoin.c"


def run(cmd, *, env=None, cwd=ROOT, check=True):
  print("$ " + " ".join(shlex.quote(str(c)) for c in cmd), flush=True)
  return subprocess.run(
    [str(c) for c in cmd], cwd=cwd, env=env, text=True, capture_output=True, check=check)


def nproc():
  try:
    return os.cpu_count() or 1
  except Exception:
    return 1


def core_sweep(spec):
  total = nproc()
  values = []
  for part in spec.split(","):
    part = part.strip().upper()
    if not part:
      continue
    if part == "N":
      value = total
    elif part == "N-2":
      value = max(1, total - 2)
    else:
      value = int(part)
    if value > 0 and value not in values:
      values.append(value)
  return values


def configure_cmake(build_dir):
  cmake = shutil.which("cmake")
  if cmake is None:
    raise RuntimeError("cmake not found")
  cache = build_dir / "CMakeCache.txt"
  if cache.exists():
    # Already configured; skip to avoid re-configure failures.
    return
  generator = ["-G", "Ninja"] if shutil.which("ninja") else []
  build_dir.mkdir(parents=True, exist_ok=True)
  run([
    cmake,
    *generator,
    "-S",
    ROOT,
    "-B",
    build_dir,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
  ])


def compile_command_for(build_dir, source):
  db = build_dir / "compile_commands.json"
  if not db.exists():
    return None
  commands = json.loads(db.read_text())
  source = str(source.resolve())
  for entry in commands:
    if str(Path(entry["file"]).resolve()) == source:
      if "arguments" in entry:
        return [str(x) for x in entry["arguments"]]
      return shlex.split(entry["command"])
  return None


def standalone_compile_from_cmake(cmd, source, output, std):
  filtered = []
  skip_next = False
  for arg in cmd:
    if skip_next:
      skip_next = False
      continue
    if arg == "-c":
      skip_next = True
      continue
    if arg == "-o":
      skip_next = True
      continue
    if arg == str(source) or arg == str(source.resolve()):
      continue
    if arg.startswith("-std="):
      continue
    if arg == "-DUSE_SYSTEMATIC_TESTING":
      continue
    filtered.append(arg)
  filtered.extend([std, str(source), "-o", str(output), "-pthread", "-latomic"])
  run(filtered)


def build_verona(build_dir, cxx):
  configure_cmake(build_dir)
  output = build_dir / "bench_forkjoin_verona"
  cmd = compile_command_for(build_dir, VERONA_SRC)
  if cmd is not None:
    standalone_compile_from_cmake(cmd, VERONA_SRC, output, "-std=c++20")
    return output

  snmalloc = build_dir / "_deps" / "snmalloc-src" / "src"
  run([
    cxx,
    "-std=c++20",
    "-O2",
    "-DNDEBUG",
    "-DSNMALLOC_CHEAP_CHECKS",
    "-I",
    ROOT / "src" / "rt",
    "-I",
    snmalloc,
    "-I",
    ROOT / "test",
    VERONA_SRC,
    "-o",
    output,
    "-pthread",
    "-latomic",
  ])
  return output


def build_cilk(build_dir, opencilk_clang):
  if opencilk_clang is None:
    return None
  output = build_dir / "bench_forkjoin_cilk"
  run([
    opencilk_clang,
    "-std=c11",
    "-fopencilk",
    "-O2",
    "-DNDEBUG",
    CILK_SRC,
    "-o",
    output,
  ])
  return output


def build_tbb_omp(build_dir, cxx):
  output = build_dir / "bench_forkjoin_tbb_omp"
  base = [cxx, "-std=c++17", "-O2", "-DNDEBUG", "-fopenmp", TBB_OMP_SRC, "-o", output]
  with_tbb = [*base[:-2], "-DUSE_TBB", *base[-2:], "-ltbb"]
  result = run(with_tbb, check=False)
  if result.returncode == 0:
    return output, True

  print(result.stderr, file=sys.stderr)
  result = run(base, check=False)
  if result.returncode == 0:
    return output, False

  print(result.stderr, file=sys.stderr)
  return None, False


def parse_output(text):
  parsed = {}
  serial_re = re.compile(r"^(fib|nqueens)\(\d+\) serial:\s+([0-9.]+) ms", re.MULTILINE)
  runtime_re = re.compile(
    r"^(fib|nqueens)\(\d+\)\s+([^:]+):\s+([0-9.]+) ms\s+\(result=.*?speedup=([0-9.]+)x\)",
    re.MULTILINE)
  for bench, ms in serial_re.findall(text):
    parsed.setdefault(bench, {})["serial_ms"] = float(ms)
  for bench, label, ms, speedup in runtime_re.findall(text):
    label = label.strip().lower()
    if label == "v-coro":
      label = "verona-coro"
    parsed.setdefault(bench, {})[label] = {
      "ms": float(ms),
      "speedup": float(speedup),
    }
  return parsed


def run_benchmark(exe, args, *, env=None):
  result = run([exe, *args], env=env)
  if result.stdout:
    print(result.stdout)
  if result.stderr:
    print(result.stderr, file=sys.stderr)
  return parse_output(result.stdout)


def merge_result(results, core, parsed):
  slot = results.setdefault(core, {"fib": {}, "nqueens": {}})
  for bench, values in parsed.items():
    slot.setdefault(bench, {}).update(values)


def format_speedup(results, bench, label):
  if label not in results.get(bench, {}):
    return "-"
  return f"{results[bench][label]['speedup']:.2f}x"


def write_report(path, args, cores, results, have_tbb, opencilk_clang):
  lines = []
  lines.append("# Fork/Join Benchmark: Cilk vs Verona vs TBB vs OpenMP")
  lines.append("")
  lines.append(f"Platform: {os.uname().machine}, {nproc()} logical cores")
  lines.append(f"Settings: fib({args.fib_n}) cutoff={args.fib_cutoff}, nqueens({args.nq_n}) cutoff={args.nq_cutoff}, best of {args.reps} reps")
  lines.append(f"OpenCilk clang: {opencilk_clang or 'not available'}")
  lines.append(f"TBB: {'available' if have_tbb else 'not available'}")
  lines.append("")
  for bench in ["fib", "nqueens"]:
    title = f"{bench} speedup over each runtime's serial baseline"
    lines.append(f"## {title}")
    lines.append("")
    lines.append("| Cores | Cilk | Verona-coro | TBB | OpenMP |")
    lines.append("|---:|---:|---:|---:|---:|")
    for core in cores:
      row = results.get(core, {})
      lines.append(
        f"| {core} | {format_speedup(row, bench, 'cilk')} | "
        f"{format_speedup(row, bench, 'verona-coro')} | "
        f"{format_speedup(row, bench, 'tbb')} | "
        f"{format_speedup(row, bench, 'openmp')} |")
    lines.append("")
  path.write_text("\n".join(lines))
  print(f"Wrote {path}")


def main():
  parser = argparse.ArgumentParser(description="Run fork/join comparison benchmarks.")
  parser.add_argument("--build-dir", type=Path, default=ROOT / "build_forkjoin_compare")
  parser.add_argument("--opencilk-clang", default=os.environ.get("OPENCILK_CLANG"))
  parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
  parser.add_argument("--cores", default="1,2,4,8,N-2,N")
  parser.add_argument("--fib_n", type=int, default=42)
  parser.add_argument("--nq_n", type=int, default=13)
  parser.add_argument("--fib_cutoff", type=int, default=20)
  parser.add_argument("--nq_cutoff", type=int, default=10)
  parser.add_argument("--reps", type=int, default=5)
  parser.add_argument("--report", type=Path, default=SCRATCH / "forkjoin_comparison_report.md")
  parser.add_argument("--skip-verona", action="store_true")
  parser.add_argument("--skip-tbb-omp", action="store_true")
  args = parser.parse_args()

  args.build_dir = args.build_dir.resolve()
  args.build_dir.mkdir(parents=True, exist_ok=True)
  cores = core_sweep(args.cores)
  common_args = [
    "--fib_n",
    args.fib_n,
    "--nq_n",
    args.nq_n,
    "--fib_cutoff",
    args.fib_cutoff,
    "--nq_cutoff",
    args.nq_cutoff,
    "--reps",
    args.reps,
  ]

  results = {}
  verona = None if args.skip_verona else build_verona(args.build_dir, args.cxx)
  cilk = build_cilk(args.build_dir, args.opencilk_clang)
  tbb_omp, have_tbb = (None, False) if args.skip_tbb_omp else build_tbb_omp(args.build_dir, args.cxx)

  if cilk is None:
    print("OpenCilk clang not provided; skipping Cilk. Set OPENCILK_CLANG or pass --opencilk-clang.")

  for core in cores:
    print(f"\n=== cores={core} ===")
    if verona is not None:
      parsed = run_benchmark(verona, ["--cores", core, "--seed", 1, *common_args])
      merge_result(results, core, parsed)
    if cilk is not None:
      env = os.environ.copy()
      env["CILK_NWORKERS"] = str(core)
      parsed = run_benchmark(cilk, common_args, env=env)
      merge_result(results, core, parsed)
    if tbb_omp is not None:
      parsed = run_benchmark(tbb_omp, ["--cores", core, *common_args])
      merge_result(results, core, parsed)

  write_report(args.report, args, cores, results, have_tbb, args.opencilk_clang)


if __name__ == "__main__":
  main()