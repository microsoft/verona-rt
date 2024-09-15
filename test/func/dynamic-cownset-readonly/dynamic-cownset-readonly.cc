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

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  when(read(t1)) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_span_empty()
{
  Logging::cout() << "test_span_empty()" << Logging::endl;

  cown_array<Body1> t1{nullptr, 0};

  when(read(t1)) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_span_single()
{
  Logging::cout() << "test_span_single()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);

  cown_array<Body1> t1{&log1, 1};

  when(read(t1)) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_multi_span()
{
  Logging::cout() << "test_multi_span()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray1[2];
  carray1[0] = log1;
  carray1[1] = log2;

  cown_array<Body1> t1{carray1, 2};

  auto log3 = make_cown<Body1>(3);
  auto log4 = make_cown<Body1>(4);

  cown_ptr<Body1> carray2[2];
  carray2[0] = log3;
  carray2[1] = log4;

  cown_array<Body1> t2{carray2, 2};

  when(read(t1), read(t2)) <<
    [=](auto, auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_mixed1()
{
  Logging::cout() << "test_mixed1()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  auto log3 = make_cown<Body1>(1);

  when(read(t1), log3) <<
    [=](auto, auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_mixed2()
{
  Logging::cout() << "test_mixed2()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  auto log3 = make_cown<Body1>(1);

  when(read(log3), t1) <<
    [=](auto, auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_mixed3()
{
  Logging::cout() << "test_mixed3()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray1[2];
  carray1[0] = log1;
  carray1[1] = log2;

  cown_array<Body1> t1{carray1, 2};

  auto log3 = make_cown<Body1>(3);
  auto log4 = make_cown<Body1>(4);

  cown_ptr<Body1> carray2[2];
  carray2[0] = log3;
  carray2[1] = log4;

  cown_array<Body1> t2{carray2, 2};

  auto log5 = make_cown<Body1>(4);

  when(read(t1), log5, read(t2))
    << [=](auto, auto, auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_mixed4()
{
  Logging::cout() << "test_mixed4()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray1[2];
  carray1[0] = log1;
  carray1[1] = log2;

  cown_array<Body1> t1{carray1, 2};

  auto log3 = make_cown<Body1>(3);
  auto log4 = make_cown<Body1>(4);

  when(read(log3), t1, read(log4))
    << [=](auto, auto, auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_multi()
{
  Logging::cout() << "test_multi()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  (when(read(t1)) << [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(read(log1)) <<
     [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_nest1()
{
  Logging::cout() << "test_nest1()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  when(read(t1)) << [=](auto) {
    when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
  };
}

void test_nest2()
{
  Logging::cout() << "test_nest2()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1(carray, 2);

  when(log1) << [=](auto) {
    when(read(t1)) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
  };
}

void test_move()
{
  Logging::cout() << "test_move()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);
  auto log2 = make_cown<Body1>(2);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log2;

  cown_array<Body1> t1{carray, 2};

  when(std::move(read(t1)))
    << [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_repeated_cown()
{
  Logging::cout() << "test_repeated_cown()" << Logging::endl;

  auto log1 = make_cown<Body1>(1);

  cown_ptr<Body1> carray[2];
  carray[0] = log1;
  carray[1] = log1;

  cown_array<Body1> t1{carray, 2};

  when(std::move(read(t1)))
    << [=](auto) { Logging::cout() << "log" << Logging::endl; };
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

  // TODO: Test case disabled as chain of reads is not implemented.
  //harness.run(test_multi);

  harness.run(test_nest1);
  harness.run(test_nest2);

  harness.run(test_move);

  harness.run(test_repeated_cown);

  return 0;
}
