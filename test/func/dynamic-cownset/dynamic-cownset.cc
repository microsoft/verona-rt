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

void test_body()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> cown_array[2];
  cown_array[0] = log1;
  cown_array[1] = log2;

  cown_span<Body1> t1{cown_array, 2};

  auto log3 = make_cown<Body2>(1);

  cown_span<Body2> t2{&log3, 1};

  when(t1, t2) <<
    [=](acquired_cown_span<Body1> tp1, acquired_cown_span<Body2> tp2) {
      Logging::cout() << "two runtime cowns" << Logging::endl;
      auto* cowns = tp1.array;
      Logging::cout() << "first: " << cowns[0]->val << Logging::endl;
      Logging::cout() << "second: " << cowns[1]->val << Logging::endl;
      auto* cowns2 = tp2.array;
      Logging::cout() << "third: " << cowns2[0]->val << Logging::endl;
    };

  when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body);

  return 0;
}
