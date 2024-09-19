// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <iostream>
#include <snmalloc/snmalloc.h>
#include <verona.h>

/**
 * Minimal test of the scheduler. Build a fibinacci sequence of scheduling
 * to test correct nesting of scheduling work.
 */

using namespace verona::rt;

using Scheduler = ThreadPool<SchedulerThread>;

std::atomic<size_t> count{0};

void run(int i)
{
  if (i > 0)
  {
    count++;
    auto w = Closure::make([i](Work* w) {
      count--;
      yield();
      Logging::cout() << "Hello from w" << i << Logging::endl;

      run(i - 1);
      run(i - 2);
      return true;
    });
    Scheduler::schedule(w);
  }
}

int main()
{
  // TODO: Make this use SystematicTestHarness.
  // Can't currently as Cown is broken.

  auto& scheduler = Scheduler::get();

  Logging::enable_logging();

  scheduler.init(4);

  run(10);

  scheduler.run();

  if (count != 0)
  {
    std::cout << "Count is not zero" << std::endl;
    abort();
  }

  heap::debug_check_empty();

  return 0;
}
