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

/**
 * Used to check for data races
 *
 */
std::atomic<int> status{0};

void add_writer()
{
  auto old_status = status.exchange(-1);
  check(old_status == 0);
}

void remove_writer()
{
  auto old_status = status.exchange(0);
  check(old_status == -1);
}

void add_reader()
{
  auto old_status = status.fetch_add(1);
  check(old_status >= 0);
}

void remove_reader()
{
  auto old_status = status.fetch_sub(1);
  check(old_status > 0);
}

using namespace verona::cpp;

void create_writer(cown_ptr<Body> c, size_t i)
{
  when() << [i, c]() {
    when(c) << [=](auto) {
      add_writer();
      Logging::cout() << "write " << i << Logging::endl;
      Systematic::yield();
      remove_writer();
    };
  };
}

void create_reader(cown_ptr<Body> c, size_t i)
{
  when() << [i, c]() {
    when(read(c)) << [=](auto) {
      add_reader();
      Logging::cout() << "read " << i << Logging::endl;
      Systematic::yield();
      remove_reader();
    };
  };
}

void test_body(size_t n)
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto c = make_cown<Body>();

  for (size_t i = 0; i < n; i++)
  {
    if (Systematic::coin(1) == 0)
      create_writer(c, i);
    else
      create_reader(c, i);
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  auto n = harness.opt.is<size_t>("--n", 7);

  harness.run(test_body, n);

  return 0;
}
