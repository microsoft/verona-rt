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
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();
  auto log2 = make_cown<Body>();

  (when(log) <<
   [=](auto b) {
     for (int i = 0; i < 10; i++)
     {
       std::cout << "Behaviour 1\n";
       sleep(1);
     }
   }) +
    (when(log2) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        std::cout << "Behaviour 2\n";
        sleep(1);
      }
    });
}

void test_body_same()
{
  Logging::cout() << "test_body_same()" << Logging::endl;

  auto log = make_cown<Body>();

  (when(log) <<
   [=](auto b) {
     for (int i = 0; i < 10; i++)
     {
       std::cout << "Behaviour 1\n";
       sleep(1);
     }
   }) +
    (when(log) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        std::cout << "Behaviour 2\n";
        sleep(1);
      }
    });

  std::cout << "Foo\n";
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body);
  harness.run(test_body_same);

  return 0;
}
