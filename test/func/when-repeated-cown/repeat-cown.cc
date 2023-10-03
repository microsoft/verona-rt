// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

void test_acquire_cown_twice()
{
  Logging::cout() << "test_acquire_cown_twice()" << Logging::endl;

  auto log = make_cown<int>(2);
  auto alog = make_cown<int>(3);

  when(log) << [=](auto) {
    Logging::cout() << "first log" << Logging::endl;
  };

  when(log, log) << [=](auto, auto) {
    Logging::cout() << "second log" << Logging::endl;
  };

  when(log, alog, log) << [=](auto, auto, auto) {
    Logging::cout() << "third log" << Logging::endl;
  };

  when(log) << [=](auto) {
    Logging::cout() << "final log" << Logging::endl;
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_acquire_cown_twice);

  return 0;
}