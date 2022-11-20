// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
namespace notify_basic
{
  bool g_called = false;

  struct A : public VCown<A>
  {
    void notified(Object* o)
    {
      auto a = (A*)o;
      (void)a;
      g_called = true;
    }
  };

  A* g_a = nullptr;

  void basic_test()
  {
    auto& alloc = ThreadAlloc::get();

    g_a = new A;

    notify(g_a);
    schedule_lambda(g_a, []() {});

    Cown::release(alloc, g_a);
  }
}
