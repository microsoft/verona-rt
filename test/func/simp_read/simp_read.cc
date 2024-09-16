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

  auto c = make_cown<Body>();

  when () << [c]() {
    when(c) <<
      [=](auto) { Logging::cout() << "write 1" << Logging::endl; };
  };

  when () << [c]() {
    when(read(c)) <<
      [=](auto) { Logging::cout() << "read 1" << Logging::endl; };
  };

  when () << [c]() {
    when(c) <<
      [=](auto) { Logging::cout() << "write 2" << Logging::endl; };
  };

  when () << [c]() {
    when(read(c)) <<
      [=](auto) { Logging::cout() << "read 2" << Logging::endl; };
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body);

  return 0;
}
