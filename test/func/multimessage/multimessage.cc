// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <debug/harness.h>
#include <debug/log.h>
#include <test/opt.h>
#include <verona.h>
#include <cpp/when.h>

using namespace snmalloc;
using namespace verona::rt;
using namespace verona::cpp;

void test_multimessage(size_t cores)
{
  struct CCown
  {
    int i = 0;

    CCown(int i) : i(i) {}

    ~CCown()
    {
      Logging::cout() << "Cown " << (void*)this << " destroyed!" << Logging::endl;
    }
  };

  Scheduler& sched = Scheduler::get();
  sched.init(cores);

  {
    auto a1 = make_cown<CCown>(3);
    when (a1) << [](auto a) { Logging::cout() << "got message on " << a.cown() << Logging::endl; };

    auto a2 = make_cown<CCown>(5);

    // We are transfering our cown references to the message here.
    when (a1, a2) << [](auto a, auto b) {
      Logging::cout() << "result = " << (a->i + b->i) << Logging::endl;
    };
  }
  sched.run();
  snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
}

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);
  test_multimessage(opt.is<size_t>("--cores", 4));
  return 0;
}
