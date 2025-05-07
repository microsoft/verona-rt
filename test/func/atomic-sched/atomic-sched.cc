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

template<bool r, typename T, typename F>
auto long_chain_helper(T obj, F&& body)
{
  if constexpr (r == true)
  {
    return when(read(obj), std::forward<F>(body));
  }
  else
  {
    return when(obj, std::forward<F>(body));
  }
}

template<bool r1>
auto make_var_chain(cown_ptr<Body> log, size_t n = 1)
{
  return long_chain_helper<r1>(log, [=](auto b) {
    for (int i = 0; i < 10; i++)
    {
      Logging::cout() << "Behaviour " << n << Logging::endl;
      Systematic::yield();
      // sleep(1);
    }
  });
}

template<bool r1, bool r2, bool... rs>
auto make_var_chain(cown_ptr<Body> log, size_t n = 1)
{
  return (long_chain_helper<r1>(
           log,
           [=](auto b) {
             for (int i = 0; i < 10; i++)
             {
               Logging::cout() << "Behaviour " << n << Logging::endl;
               Systematic::yield();
               // sleep(1);
             }
           })) +
    (make_var_chain<r2, rs...>(log, n + 1));
}

void test_body()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();
  auto log2 = make_cown<Body>();

  (when(
    log,
    [=](auto b) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 1\n";
        // sleep(1);
      }
    })) +
    (when(log2, [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    }));
}

void test_body_read_mixed()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log = make_cown<Body>();
  auto log2 = make_cown<Body>();

  (when(
    read(log),
    [=](acquired_cown<const Body> b) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 1\n";
        // sleep(1);
      }
    })) +
    (when(log2, [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    }));
}

void test_body_smart()
{
  Logging::cout() << "test_body_smart()" << Logging::endl;

  auto log = make_cown<Body>();
  auto log2 = make_cown<Body>();
  auto ptr = std::make_unique<int>(42);

  (when(
    log,
    [=, ptr = std::move(ptr)](auto b) {
      std::cout << "ptr = " << *ptr << std::endl;
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 1\n";
        // sleep(1);
      }
    })) +
    (when(log2, [=](auto) {
      for (int i = 0; i < 10; i++)
      {
        Logging::cout() << "Behaviour 2\n";
        // sleep(1);
      }
    }));
}

void test_body_concurrent_1()
{
  auto log = make_cown<Body>();

  when([=]() { make_var_chain<false, true>(log); });

  when([=]() { make_var_chain<true, false>(log); });
}

void test_body_concurrent_2()
{
  auto log = make_cown<Body>();

  when([=]() { make_var_chain<false, true, false>(log); });

  when([=]() { make_var_chain<true, false, true>(log); });
}

template<bool r1, bool... rs>
void test_body_long_chain_var()
{
  auto log = make_cown<Body>();

  make_var_chain<r1, rs...>(log);
}

auto repeat_shape = [](auto c1, auto c2, auto c3) {
  return (when(
           c1,
           c2,
           [=](auto b1, auto b2) {
             for (int i = 0; i < 10; i++)
             {
               Logging::cout() << "Behaviour 1\n";
               // sleep(1);
             }
           })) +
    (when(c3, [=](auto b) {
           for (int i = 0; i < 10; i++)
           {
             Logging::cout() << "Behaviour 2\n";
             // sleep(1);
           }
         }));
};

void test_body_repeat1()
{
  auto log = make_cown<Body>();
  repeat_shape(log, log, log);
}

void test_body_repeat2()
{
  auto log = make_cown<Body>();
  repeat_shape(log, log, read(log));
}

void test_body_repeat3()
{
  auto log = make_cown<Body>();
  repeat_shape(log, read(log), log);
}

void test_body_repeat4()
{
  auto log = make_cown<Body>();
  repeat_shape(log, read(log), read(log));
}

void test_body_repeat5()
{
  auto log = make_cown<Body>();
  repeat_shape(read(log), log, log);
}

void test_body_repeat6()
{
  auto log = make_cown<Body>();
  repeat_shape(read(log), log, read(log));
}

void test_body_repeat7()
{
  auto log = make_cown<Body>();
  repeat_shape(read(log), read(log), log);
}

void test_body_repeat8()
{
  auto log = make_cown<Body>();
  repeat_shape(read(log), read(log), read(log));
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run_many(
    {{test_body, "test_body"},
     {test_body_smart, "test_body_smart"},
     {test_body_read_mixed, "test_body_read_mixed"},
     {test_body_concurrent_1, "test_body_concurrent_1"},
     {test_body_concurrent_2, "test_body_concurrent_2"},
     // Two long chains
     {test_body_long_chain_var<true, false>,
      "test_body_long_chain_var<true, false>"},
     {test_body_long_chain_var<false, true>,
      "test_body_long_chain_var<false, true>"},
     {test_body_long_chain_var<false, false>,
      "test_body_long_chain_var<false, false>"},
     {test_body_long_chain_var<true, true>,
      "test_body_long_chain_var<true, true>"},
     // Three long chains
     {test_body_long_chain_var<true, true, true>,
      "test_body_long_chain_var<true, true, true>"},
     {test_body_long_chain_var<true, true, false>,
      "test_body_long_chain_var<true, true, false>"},
     {test_body_long_chain_var<true, false, true>,
      "test_body_long_chain_var<true, false, true>"},
     {test_body_long_chain_var<true, false, false>,
      "test_body_long_chain_var<true, false, false>"},
     {test_body_long_chain_var<false, true, true>,
      "test_body_long_chain_var<false, true, true>"},
     {test_body_long_chain_var<false, true, false>,
      "test_body_long_chain_var<false, true, false>"},
     {test_body_long_chain_var<false, false, true>,
      "test_body_long_chain_var<false, false, true>"},
     {test_body_long_chain_var<false, false, false>,
      "test_body_long_chain_var<true, false, true>"},
     // Four long chains
     {test_body_long_chain_var<false, true, false, true>,
      "test_body_long_chain_var<false, true, false, true>"},
     {test_body_long_chain_var<true, false, true, false>,
      "test_body_long_chain_var<true, false, true, false>"},
     // Repeat shapes.
     {test_body_repeat1, "test_body_repeat1"},
     {test_body_repeat2, "test_body_repeat2"},
     {test_body_repeat3, "test_body_repeat3"},
     {test_body_repeat4, "test_body_repeat4"},
     {test_body_repeat5, "test_body_repeat5"},
     {test_body_repeat6, "test_body_repeat6"},
     {test_body_repeat7, "test_body_repeat7"},
     {test_body_repeat8, "test_body_repeat8"}});

  return 0;
}
