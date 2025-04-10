// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

class Body
{
public:
  ~Body()
  {
    Logging::cout() << "Body destroyed" << Logging::endl;
  }
};

using namespace verona::cpp;

void test_body_move()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();

  when(std::move(log)) <<
    [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_body_move_busy()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();

  when(log) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
  when(std::move(log)) <<
    [=](auto) { Logging::cout() << "log" << Logging::endl; };
}

void test_sched_many_no_move()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = cown_ptr<Body>(log1);

  (when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(log2) << [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_sched_many_no_move_busy()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = cown_ptr<Body>(log1);

  when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
  (when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(log2) << [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_sched_many_move()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = make_cown<Body>();

  (when(std::move(log1)) <<
   [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(std::move(log2)) <<
     [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_sched_many_move_busy()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = make_cown<Body>();

  when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
  (when(std::move(log1)) <<
   [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(std::move(log2)) <<
     [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_sched_many_mixed()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = make_cown<Body>();

  (when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(std::move(log2)) <<
     [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_sched_many_mixed_busy()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = make_cown<Body>();

  when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; };
  (when(log1) << [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(std::move(log2)) <<
     [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

void test_sched_many_move_same()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();
  auto log2 = cown_ptr<Body>(log1);

  (when(std::move(log1)) <<
   [=](auto) { Logging::cout() << "log" << Logging::endl; }) +
    (when(std::move(log2)) <<
     [=](auto) { Logging::cout() << "log" << Logging::endl; });
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body_move);
  harness.run(test_body_move_busy);
  harness.run(test_sched_many_no_move);
  harness.run(test_sched_many_no_move_busy);
  harness.run(test_sched_many_move);
  harness.run(test_sched_many_move_busy);
  harness.run(test_sched_many_mixed);
  harness.run(test_sched_many_mixed_busy);
  harness.run(test_sched_many_move_same);

  return 0;
}
