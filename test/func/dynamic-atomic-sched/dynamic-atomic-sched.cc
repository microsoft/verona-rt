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

void test_body()
{
  DynamicAtomicBatch dab;
  auto log = make_cown<Body>();

  for (int i = 0; i < 2; i++)
  {
    dab + (when(log) << [=](auto b) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 1" << Logging::endl;
      }
    });
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body);

  return 0;
}
