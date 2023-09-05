// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

class Body
{
public:
  int counter;
};

using namespace verona::cpp;

void test_body()
{
  auto log1 = make_cown<Body>();
  auto log2 = make_cown<Body>();

  when(log2) << [](auto l) {
    Logging::cout() << "Short running task starting ..... " << Logging::endl;
    Logging::cout() << "Short running task finished counter = "
                    << Logging::endl;
  };

  when(log1) << [](auto l) {
    Logging::cout() << "Long running task starting counter value = "
                    << l->counter << Logging::endl;

    while (l->counter < 100 && !verona::rt::behaviour_yielded)
    {
      l->counter++;
      if (l->counter % 10 == 0)
      {
        Logging::cout() << "Yielding at counter = " << l->counter
                        << Logging::endl;
        BEHAVIOUR_YIELD();
      }
    }
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  Logging::cout() << "Yield test starting" << Logging::endl;

  harness.run(test_body);

  return 0;
}
