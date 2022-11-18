// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <ctime>
#include <debug/harness.h>
#include <cpp/when.h>

struct Runner
{
  Runner() {}
};

using namespace verona::cpp;

void schedule_run(size_t decay)
{
  if (decay == 0)
    return;

  auto& alloc = ThreadAlloc::get();
  auto runner = make_cown<Runner>();
  when (runner) << [decay](auto) { schedule_run(decay - 1); };
}

void basic_test(size_t cores)
{
  // There should be one fewer runners than cores to cause
  // stealing to occur a lot.
  for (size_t i = 0; i < cores - 1; i++)
  {
    schedule_run(3);
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);
  harness.run(basic_test, harness.cores);
  return 0;
}
