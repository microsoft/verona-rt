// Copyright Microsoft and Project Verona Contributors
// SPDX-License-Identifier: MIT

// See issue #84 for origin of this test.
// This is probably no longer relevant as the teardown without the
// leak detector is much simpler.  Leaving the test incase that
// changes.

#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
extern "C" const char* __asan_default_options()
{
  return "detect_leaks=0";
}
#  endif
#endif

struct MyCown
{};

cown_ptr<MyCown>::weak weak_leak;

void run_test()
{
  auto t = make_cown<MyCown>();
  // HERE: the weak RC is never released.
  weak_leak = t.get_weak();

  when(t) <<
    [](auto t) { Logging::cout() << "Msg on " << t.cown() << std::endl; };
}

int main(int argc, char** argv)
{
  SystematicTestHarness h(argc, argv);

  h.detect_leaks = false;

  h.run(run_test);
}
