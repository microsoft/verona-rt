// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <debug/harness.h>
#include <random>
#include <test/opt.h>
#include <verona.h>

using namespace snmalloc;
using namespace verona::cpp;
using namespace verona::rt;

struct A
{};

void test_runtime_pause(SystematicTestHarness* harness, size_t pauses)
{
  when([harness, pauses]() {
    auto a = make_cown<A>();
    Scheduler::add_external_event_source();
    auto pauses_ = pauses;
    harness->external_thread([pauses_, a]() mutable {
      Logging::cout() << "Started external thread" << Logging::endl;
      std::mt19937 rng;
      rng.seed(1);
      std::uniform_int_distribution<> dist(1, 1000);
      for (size_t i = 1; i <= pauses_; i++)
      {
        auto pause_time = std::chrono::milliseconds(dist(rng));
        std::this_thread::sleep_for(pause_time);
        Logging::cout() << "Scheduling Message" << Logging::endl;
        when(a, [i](auto) {
          Logging::cout() << "running message " << i << std::endl;
        });
      }

      // We need to clear out reference count, before we
      // `remove_external_event_source`s.
      a.clear();

      when([]() {
        Logging::cout() << "Remove external event source" << std::endl;
        Scheduler::remove_external_event_source();
      });

      Logging::cout() << "External thread exiting" << Logging::endl;
    });
  });
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  size_t pauses = harness.opt.is<size_t>("--pauses", 3);

  harness.run(test_runtime_pause, &harness, pauses);
  return 0;
}
