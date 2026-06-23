// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include "rc_release_lins_double_free.h"

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

  rc_release_lins_double_free::run_test();

  return 0;
}