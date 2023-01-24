// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <debug/harness.h>
// Harness must come before tests.
#include "./notify_alternate.h"
#include "./notify_basic.h"
#include "./notify_interleave.h"

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(notify_basic::basic_test);

  Logging::cout() << "Non-interleaved test" << Logging::endl;
  harness.run(notify_interleave::run_test, false);

  Logging::cout() << "Interleaved test" << Logging::endl;
  harness.run(notify_interleave::run_test, true);

  harness.run(notify_empty_queue::run_test);

  // TODO: Notify coalesce is broken. We need to correctly design this
  // feature for the behaviour centric scheduling.
  // // Here we ensure single-core so that we can check the number of times
  // // `notified` is called.
  // if (harness.cores == 1)
  //   harness.run(notify_coalesce::run_test);

  return 0;
}
