// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include "rc_distant_cycle.h"

#include <debug/harness.h>
#include <test/opt.h>

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);

#ifdef CI_BUILD
  auto log = true;
#else
  auto log = opt.has("--log-all");
#endif

  if (log)
    Logging::enable_logging();

  rc_distant_cycle::run_test();

  return 0;
}