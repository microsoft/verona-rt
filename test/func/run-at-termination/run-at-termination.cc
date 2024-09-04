// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

/**
 * Check if thread_local variables are correctly added to global variable
 * once the thread finishes.
 *
 * Create NUM_OPS behaviour where each behaviour increments the thread_local ops
 * variable by NUM_OPS At thread termination, add the ops variable to the
 * total_ops variable. Finally, check if the total_ops variable == NUM_OPS *
 * NUM_OPS
 */

static constexpr uint64_t NUM_OPS = 10;

// How many scheduler cores are being used.
size_t cores{0};

// Thread local variable to store the number of operations.
thread_local long ops{0};

using namespace verona::cpp;

void test_body()
{
  // Run NUM_OPS behaviours, each incrementing ops by NUM_OPS.
  for (int i = 0; i < NUM_OPS; i++)
  {
    when() << []() {
      for (int i = 0; i < NUM_OPS; i++)
        ops++;
    };
  }
}

void finish(void)
{
  static std::atomic<size_t> finished_count{0};
  static std::atomic<size_t> total_ops{0};

  // Add the thread local ops to the total_ops.
  total_ops.fetch_add(ops);
  // Reset thread local for subsequent runs.
  ops = 0;

  // Increment the finished count.
  auto prev = finished_count.fetch_add(1);

  // Check if was the last core to finish.
  if (prev != cores - 1)
    return;

  // Actual check.
  check(total_ops.load() == (NUM_OPS * NUM_OPS));

  // Reset the counters for the next run.
  finished_count = 0;
  total_ops = 0;
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  cores = harness.cores;

  harness.run_at_termination = finish;
  harness.run(test_body);

  return 0;
}
