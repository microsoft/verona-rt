// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/coro.h>
#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

class Body
{
public:
  int counter;

  ~Body()
  {
    Logging::cout() << "Body destroyed" << Logging::endl;
  }
};

void test_body()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();

  when() << []() { std::cout << "in between\n"; };

  when(log1) << [=](acquired_cown<Body>& acq) mutable -> coroutine {
    std::cout << "counter = " << acq->counter << std::endl;
    int local_counter = 0;

    acq->counter++;
    local_counter++;
    // Yield the execution
    co_await std::experimental::suspend_always{};

    std::cout << "counter = " << acq->counter << std::endl;
    std::cout << "local counter = " << local_counter << std::endl;
    std::cout << "end" << std::endl;
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  Logging::cout() << "Yield test" << Logging::endl;

  harness.run(test_body);

  return 0;
}
