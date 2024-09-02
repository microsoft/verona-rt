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
 *
 * NOTE: Run with seed_count of 1, otherwise the condition will be false
 * (harness.run() will execute test_body() with multiple seeds before
 * returning).
 */

static constexpr uint64_t NUM_OPS = 10;

thread_local long ops = 0;
std::atomic<long> total_ops = 0;

using namespace verona::cpp;

void test_body()
{
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
  total_ops.fetch_add(ops);
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run_at_termination = finish;
  harness.run(test_body);

  Logging::cout() << "total_ops actual: " << total_ops.load()
                  << " expected: " << (NUM_OPS * NUM_OPS) << Logging::endl;
  if (total_ops.load() != (NUM_OPS * NUM_OPS))
    abort();

  return 0;
}
