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
       Logging::cout() << "Behaviour 1\n";
       // sleep(1);
     }
   }) +
    (when(log2) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    });
}

void test_body_read_mixed()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();
  auto log2 = make_cown<Body>();

  (when(read(log)) <<
   [=](acquired_cown<const Body> b) {
     for (int i = 0; i < 10; i++)
     {
       Logging::cout() << "Behaviour 1\n";
       // sleep(1);
     }
   }) +
    (when(log2) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    });
}

void test_body_read_same1()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();

  (when(read(log)) <<
   [=](acquired_cown<const Body> b) {
     for (int i = 0; i < 10; i++)
     {
       Logging::cout() << "Behaviour 1\n";
       // sleep(1);
     }
   }) +
    (when(log) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    });
}

void test_body_read_same2()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();

  (when(log) <<
   [=](auto b) {
     for (int i = 0; i < 10; i++)
     {
       Logging::cout() << "Behaviour 1\n";
       // sleep(1);
     }
   }) +
    (when(read(log)) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    });
}

void test_body_read_only_same()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();

  (when(read(log)) <<
   [=](acquired_cown<const Body> b) {
     for (int i = 0; i < 10; i++)
     {
       Logging::cout() << "Behaviour 1\n";
       // sleep(1);
     }
   }) +
    (when(read(log)) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
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
       Logging::cout() << "Behaviour 1" << Logging::endl;
     }
   }) +
    (when(log) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2" << Logging::endl;
      }
    });
}

void test_body_smart()
{
  Logging::cout() << "test_body_smart()" << Logging::endl;

  auto log = make_cown<Body>();
  auto log2 = make_cown<Body>();
  auto ptr = std::make_unique<int>(42);

  (when(log) <<
   [=, ptr = std::move(ptr)](auto b) {
     std::cout << "ptr = " << *ptr << std::endl;
     for (int i = 0; i < 10; i++)
     {
       Logging::cout() << "Behaviour 1\n";
       // sleep(1);
     }
   }) +
    (when(log2) << [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    });
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  // harness.run(test_body);
  // harness.run(test_body_read_mixed);
  harness.run(test_body_read_only_same);
  // harness.run(test_body_read_same1);
  // harness.run(test_body_read_same2);
  // harness.run(test_body_same);
  // harness.run(test_body_smart);

  return 0;
}
