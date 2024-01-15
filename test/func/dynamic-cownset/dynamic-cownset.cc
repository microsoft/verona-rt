// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>
#include <tuple>

class Body
{
public:
  int val;
  Body(int val_) : val(val_) {}

  ~Body()
  {
    Logging::cout() << "Body destroyed" << Logging::endl;
  }
};

using namespace verona::cpp;

void test_span()
{
  Logging::cout() << "test_span()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1{carray, 2};

  when(t1) << [=](acquired_cown_span<Body>) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_span_empty()
{
  Logging::cout() << "test_span_empty()" << Logging::endl;

  cown_array<Body> t1{nullptr, 0};

  when(t1) << [=](acquired_cown_span<Body>) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_span_single()
{
  Logging::cout() << "test_span_single()" << Logging::endl;

  auto log1 = make_cown<Body>(1);

  cown_array<Body> t1{&log1, 1};

  when(t1) << [=](acquired_cown_span<Body>) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_multi_span()
{
  Logging::cout() << "test_multi_span()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray1[2];
  carray1[0] = log1;
  carray1[1] = log2;

  cown_array<Body> t1{carray1, 2};

  auto log3 = make_cown<Body>(3);
  auto log4 = make_cown<Body>(4);

  cown_ptr<Body> carray2[2];
  carray2[0] = log3;
  carray2[1] = log4;

  cown_array<Body> t2{carray2, 2};

  when(t1, t2) << [=](acquired_cown_span<Body>, acquired_cown_span<Body>) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_mixed1()
{
  Logging::cout() << "test_mixed1()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1{carray, 2};

  auto log3 = make_cown<Body>(1);

  when(t1, log3) << [=](acquired_cown_span<Body> ca, acquired_cown<Body> a) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_mixed2()
{
  Logging::cout() << "test_mixed2()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1{carray, 2};

  auto log3 = make_cown<Body>(1);

  when(log3, t1) << [=](acquired_cown<Body>, acquired_cown_span<Body> ca) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_mixed3()
{
  Logging::cout() << "test_mixed3()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray1[2];
  carray1[0] = log1;
  carray1[1] = log2;

  cown_array<Body> t1{carray1, 2};

  auto log3 = make_cown<Body>(3);
  auto log4 = make_cown<Body>(4);

  cown_ptr<Body> carray2[2];
  carray2[0] = log3;
  carray2[1] = log4;

  cown_array<Body> t2{carray2, 2};

  auto log5 = make_cown<Body>(4);

  when(t1, log5, t2) <<
    [=](
      acquired_cown_span<Body>, acquired_cown<Body>, acquired_cown_span<Body>) {
      Logging::cout() << "log" << Logging::endl;
    };
}

void test_mixed4()
{
  Logging::cout() << "test_mixed4()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray1[2];
  carray1[0] = log1;
  carray1[1] = log2;

  cown_array<Body> t1{carray1, 2};

  auto log3 = make_cown<Body>(3);
  auto log4 = make_cown<Body>(4);

  when(log3, t1, log4) <<
    [=](acquired_cown<Body>, acquired_cown_span<Body>, acquired_cown<Body>) {
      Logging::cout() << "log" << Logging::endl;
    };
}

void test_multi()
{
  Logging::cout() << "test_multi()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1{carray, 2};

  (when(t1) <<
   [=](acquired_cown_span<Body>) {
     Logging::cout() << "log" << Logging::endl;
   }) +
    (when(log1) <<
     [=](acquired_cown<Body>) { Logging::cout() << "log" << Logging::endl; });
}

void test_nest1()
{
  Logging::cout() << "test_nest1()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1{carray, 2};

  when(t1) << [=](acquired_cown_span<Body>) {
    when(log1) <<
      [=](acquired_cown<Body>) { Logging::cout() << "log" << Logging::endl; };
  };
}

void test_nest2()
{
  Logging::cout() << "test_nest2()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1(carray, 2);

  when(log1) << [=](acquired_cown<Body>) {
    when(t1) << [=](acquired_cown_span<Body>) {
      Logging::cout() << "log" << Logging::endl;
    };
  };
}

void test_move()
{
  Logging::cout() << "test_span()" << Logging::endl;

  auto log1 = make_cown<Body>(1);
  auto log2 = make_cown<Body>(2);

  cown_ptr<Body> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body> t1{carray, 2};

  when(std::move(t1)) << [=](acquired_cown_span<Body>) {
    Logging::cout() << "log" << Logging::endl;
  };
}

void test_repeated_cown()
{
  Logging::cout() << "test_repeated_cown()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log1;

  cown_array<Body1> t1{carray, 2};

  when(std::move(t1)) <<
    [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_span);
  harness.run(test_span_empty);
  harness.run(test_span_single);
  harness.run(test_multi_span);

  harness.run(test_mixed1);
  harness.run(test_mixed2);
  harness.run(test_mixed3);
  harness.run(test_mixed4);

  harness.run(test_multi);

  harness.run(test_nest1);
  harness.run(test_nest2);

  harness.run(test_move);

  harness.run(test_repeated_cown);

  return 0;
}
