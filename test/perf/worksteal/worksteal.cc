// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * This benchmark is for testing performance of the work stealing code.
 *
 * A single behaviour generates all the work, which then needs dividing amongst
 * the scheduler threads.
 *
 * There are two types of work that are generated
 *   - nop: does nothing
 *   - work: schedules a an item on sync to count down to monitor the time taken
 *
 * Basic shape is
 *
 *   sync -> nop
 *        -> nop
 *        -> nop
 *        -> nop
 *        -> work -> sync
 *        ...
 *
 * The time taken to schedule all the work is measured.
 * The time taken to execute all the work is measured.
 *
 * The core aim is to generate a lot of work on a single scheduler thread to tax
 * the work stealing code.  As the main behaviour in test is long running,
 * stealing work items from the scheduler thread that runs it is essential.
 *
 * We generate a nop work so that not everything has to contend for the sync
 * cown to complete.
 */

#include "debug/log.h"
#include "test/opt.h"
#include "test/xoroshiro.h"
#include "verona.h"

#include <chrono>
#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

struct Sync
{
  high_resolution_clock::time_point start{};
  high_resolution_clock::time_point end{};
  size_t remaining_count{0};
};

void test()
{
  auto sync = verona::cpp::make_cown<Sync>();

  when(sync, [](auto sync) {
    sync->start = high_resolution_clock::now();

    sync->remaining_count = 1'000'000;

    for (size_t i = 0; i < sync->remaining_count; i++)
    {
      // Schedule some work to be done
      when([]() {});
      when([]() {});
      when([]() {});
      when([]() {});
      // Every 5th work item will be counted back in for timing purposes
      when([sync = sync.cown()]() {
        when(sync, [](auto sync) {
          // Check if this is the last work item
          if (--sync->remaining_count == 0)
          {
            sync->end = high_resolution_clock::now();
            printf(
              "Elapsed:\n\t%zu ms\n",
              duration_cast<milliseconds>(sync->end - sync->start).count());
            return;
          }
        });
      });
    }
    printf(
      "Scheduled all work took:\n\t%zu ms\n",
      duration_cast<milliseconds>(high_resolution_clock::now() - sync->start)
        .count());
  });
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test);

  return 0;
}
