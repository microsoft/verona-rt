// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <debug/harness.h>

using namespace std;

struct TestCown : public VCown<TestCown>
{};

struct A
{
  int v;
  TestCown* t;

  A(int v_) : v(v_)
  {
    t = new TestCown;
  }

  ~A()
  {
    Cown::release(t);
  }
};

void lambda_smart()
{
  auto a = make_unique<A>(42);
  schedule_lambda(
    [a = std::move(a)]() { std::cout << "Hello " << a->v << std::endl; });
}

void lambda_args()
{
  int a = 42;
  schedule_lambda(
    [=]() { std::cout << "captured arg a = " << a << std::endl; });
}

void lambda_no_cown()
{
  schedule_lambda([]() { std::cout << "Hello world!\n"; });
}

void lambda_cown()
{
  TestCown* c = new TestCown;
  schedule_lambda(c, []() { std::cout << "Hello world!\n"; });
  Cown::release(c);
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(lambda_no_cown);
  harness.run(lambda_cown);
  harness.run(lambda_args);
  harness.run(lambda_smart);

  return 0;
}
