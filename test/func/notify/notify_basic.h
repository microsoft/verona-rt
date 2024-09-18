// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
namespace notify_basic
{
  bool g_called = false;

  struct A : public VCown<A>
  {};

  A* g_a = nullptr;

  void basic_test()
  {
    auto& alloc = ThreadAlloc::get();

    g_a = new A;

    auto notify = make_notification(g_a, []() { g_called = true; });

    notify->notify();

    schedule_lambda(g_a, []() {});

    Cown::release(g_a);
    Shared::release(notify);
  }
}
