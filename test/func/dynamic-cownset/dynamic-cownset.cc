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

void test_span()
{
  Logging::cout() << "test_span()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> cown_array[2];
  cown_array[0] = log1;
  cown_array[1] = log2;

  cown_ptr_span<Body1> t1{cown_array, 2};

  when(t1) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_mixed()
{
  Logging::cout() << "test_mixed()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> cown_array[2];
  cown_array[0] = log1;
  cown_array[1] = log2;

  cown_ptr_span<Body1> t1{cown_array, 2};

  auto log3 = make_cown<Body2>(1);

  cown_ptr_span<Body2> t2{&log3, 1};

  when(t1, log1) << [=](acquired_cown_span<Body1> ca, acquired_cown<Body1> a) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_multi()
{
  Logging::cout() << "test_multi()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> cown_array[2];
  cown_array[0] = log1;
  cown_array[1] = log2;

  cown_ptr_span<Body1> t1{cown_array, 2};

  auto log3 = make_cown<Body2>(1);

  cown_ptr_span<Body2> t2{&log3, 1};

  (when(t1) << [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_span);
  harness.run(test_mixed);
  harness.run(test_multi);

  return 0;
}
