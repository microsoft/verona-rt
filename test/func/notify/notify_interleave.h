// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
namespace notify_interleave
{
  struct Ping
  {
    void operator()() {}
  };

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

  enum Phase
  {
    NOTIFYSEND,
    WAIT,
    EXIT,
  };

  struct B : public VCown<B>
  {
    A* a;
    // Here we wait for 100 msgs so that we know a has been scheduled.
    int wait_count = 100;
    Phase state = NOTIFYSEND;

    B(A* a_) : a{a_} {}

    void trace(ObjectStack& st) const
    {
      check(a);
      st.push(a);
    }
  };

  struct Loop
  {
    B* b;
    Loop(B* b) : b(b) {}

    void operator()()
    {
      auto a = b->a;
      switch (b->state)
      {
        case NOTIFYSEND:
        {
          g_called = false;
          notify(a);
          Behaviour::schedule<Ping>(a);
          b->state = WAIT;
          Behaviour::schedule<Loop>(b, b);
          break;
        }

        case WAIT:
        {
          if (b->wait_count > 0)
          {
            b->wait_count--;
          }
          else
          {
            b->state = EXIT;
          }
          Behaviour::schedule<Loop>(b, b);
          break;
        }

        case EXIT:
        {
          Cown::release(ThreadAlloc::get(), b);
          break;
        }

        default:
        {
          abort();
        }
      }
    }
  };

  // TODO Notify: Revise comment when we implement notify
  // This test confirms that `mark_notify` info is preserved when new messages
  // are sent to that cown.
  void run_test()
  {
    auto a = new A;
    auto b = new B(a);

    Behaviour::schedule<Loop>(b, b);
  }
}
