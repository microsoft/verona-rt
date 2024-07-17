// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>
#include <tuple>

class Body1
{
public:
  int val;
  Body1(int val_) : val(val_) {}

  ~Body1()
  {
    Logging::cout() << "Body1 destroyed" << Logging::endl;
  }
};

class Body2
{
public:
  int val;
  Body2(int val_) : val(val_) {}

  ~Body2()
  {
    Logging::cout() << "Body2 destroyed" << Logging::endl;
  }
};

using namespace verona::cpp;

void test_span_readonly()
{
  Logging::cout() << "test_span_readonly()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  when(read(t1)) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_span_readonly);

  return 0;
}
