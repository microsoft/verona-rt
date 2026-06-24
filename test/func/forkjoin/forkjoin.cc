// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

// Test for the Cilk-style fork/join API built on the verona-rt scheduler.

#include <cpp/forkjoin.h>
#include <cpp/when.h>
#include <debug/harness.h>

#include <atomic>
#include <cassert>
#include <vector>

using namespace verona::rt::forkjoin;
using namespace verona::cpp;

// ---- Test 1: basic fork_join_sync ----

void test_basic_fork_join()
{
  std::atomic<int> sum{0};

  when() << [&sum]() {
    fork_join_sync([&](BlockingScope& scope) {
      scope.spawn([&] { sum.fetch_add(1); });
      scope.spawn([&] { sum.fetch_add(2); });
      scope.spawn([&] { sum.fetch_add(3); });
    });

    int result = sum.load();
    Logging::cout() << "fork_join_sync result: " << result << Logging::endl;
    assert(result == 6);
  };
}

// ---- Test 2: parallel_for ----

void test_parallel_for()
{
  static constexpr size_t N = 1000;
  auto* data = new std::atomic<int>[N];
  for (size_t i = 0; i < N; i++)
    data[i].store(0);

  when() << [data]() {
    parallel_for(N, 100, [data](size_t i) { data[i].store((int)(i * 2)); });

    // Verify
    for (size_t i = 0; i < N; i++)
    {
      int expected = (int)(i * 2);
      int actual = data[i].load();
      if (actual != expected)
      {
        Logging::cout() << "FAIL parallel_for: data[" << i << "] = " << actual
                        << " expected " << expected << Logging::endl;
        assert(false);
      }
    }
    Logging::cout() << "parallel_for: OK (" << N << " elements)"
                    << Logging::endl;
    delete[] data;
  };
}

// ---- Test 3: parallel_for correctness (non-nested blocking is safe) ----

void test_parallel_for_large()
{
  static constexpr size_t N = 10000;
  auto* data = new std::atomic<int>[N];
  for (size_t i = 0; i < N; i++)
    data[i].store(0);

  when() << [data]() {
    // Single level of parallelism — safe with blocking sync
    parallel_for(N, 500, [data](size_t i) {
      data[i].store((int)(i * 3 + 1));
    });

    size_t errors = 0;
    for (size_t i = 0; i < N; i++)
    {
      if (data[i].load() != (int)(i * 3 + 1))
        errors++;
    }
    Logging::cout() << "parallel_for_large: " << errors << " errors"
                    << Logging::endl;
    assert(errors == 0);
    delete[] data;
  };
}

// ---- Test 4: continuation-based TaskFrame (safe for nesting) ----
// This is the proper Cilk-style API: spawn children, arm frame,
// return to scheduler. Continuation fires when all children complete.

static std::atomic<int> g_frame_sum{0};
static std::atomic<bool> g_cont_ran{false};

void test_task_frame()
{
  g_frame_sum.store(0);
  g_cont_ran.store(false);

  when() << []() {
    auto* frame = TaskFrame::create([]() {
      int s = g_frame_sum.load();
      Logging::cout() << "TaskFrame continuation: sum=" << s << Logging::endl;
      assert(s == 10);
      g_cont_ran.store(true, std::memory_order_release);
    });

    frame->spawn([] { g_frame_sum.fetch_add(1); });
    frame->spawn([] { g_frame_sum.fetch_add(2); });
    frame->spawn([] { g_frame_sum.fetch_add(3); });
    frame->spawn([] { g_frame_sum.fetch_add(4); });
    frame->arm();
    // After arm(), this task is done. Continuation fires asynchronously.
  };
}

// ---- Main ----

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_basic_fork_join);
  harness.run(test_parallel_for);
  harness.run(test_parallel_for_large);
  harness.run(test_task_frame);

  return 0;
}
