// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <ctime>
#include <debug/harness.h>

using namespace verona::cpp;
// Work load that is designed to cause fairness to kick in
// This does not check it is fair, just that it does not crash.
// Designed for systematic testing.

static constexpr int start_count = 100;
struct A
{
  int id;
  int count = start_count;

  A(int id_) : id{id_} {}
};

void loop(cown_ptr<A> c)
{
  when(c) << [c = std::move(c)](acquired_cown<A> a) {
    auto& count = a->count;

    if (count == 0)
    {
      return;
    }

    count--;
    loop(std::move(c));
  };
}

void basic_test()
{
  when() << []() {
    for (int i = 0; i < 6; ++i)
    {
      loop(make_cown<A>(i));
    }
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);
  harness.run(basic_test);
  return 0;
}
