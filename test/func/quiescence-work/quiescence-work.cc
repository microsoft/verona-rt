// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <debug/harness.h>
#include <sched/schedulerthread.h>
#include <sched/work.h>

using namespace verona::rt;

namespace
{
  constexpr size_t task_count = 32;

  std::atomic<size_t> phase{0};
  std::atomic<size_t> completed{0};

  void test()
  {
    phase = 0;
    completed = 0;

    auto* first = Closure::make([](Work*) {
      check(phase.exchange(2) == 1);
      check(completed.load() == task_count);
      return true;
    });

    auto* second = Closure::make([](Work*) {
      check(phase.exchange(1) == 0);

      for (size_t i = 0; i < task_count; i++)
      {
        auto* work = Closure::make([](Work*) {
          completed.fetch_add(1);
          return true;
        });
        Scheduler::schedule(work);
      }

      return true;
    });

    Scheduler::schedule_at_quiescence(first);
    Scheduler::schedule_at_quiescence(second);
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);
  harness.run(test);

  check(phase.load() == 2);
  check(completed.load() == task_count);
  return 0;
}
