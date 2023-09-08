// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * This test creates a possible deadlock between cowns c1, c2, and c3 where the
 * acquisition order of these cowns is c1 before c2 before c3. The following
 * order of events may occur:
 *  1. c1 is overloaded
 *  2. c3 creates a behaviour {c1}. c1 mutes c3.
 *  3. c2 creates a behaviour {c2, c3}. c2 is acquired and blocked on c3 until
 *     c3 is unmuted and c3 runs this behaviour.
 *  4. c1 creates a behaviour {c1, c2}. c1 is acquired and blocked on c2 until
 *     c2 is rescheduled and runs this behaviour. The priority of c2 is raised.
 *
 * In this scenario, it is possible for all three cowns to be deadlocked, unless
 * c2 is unblocked when its priority is raised by unmuting c1.
 */

/*
class C {}
class Main
{
  main()
  {
    var c1 = cown.create(new C);
    var c2 = cown.create(new C);
    var c3 = cown.create(new C);
    // overload c1
    var i = 100;
    while i > 0
    {
      i = i - 1;
      when (c1) {};
    };
    // c3 send {c1}
    when (var _ = c3) { when (c1) {} };
    // c2 send {c2, c3}
    when (var _ = c2) { when (c2, c3) {} };
    // c1 send {c1, c2}
    when (var _ = c1) { when (c1, c2) {} }
  }
}
*/

#include "cpp/when.h"
#include "verona.h"

namespace backpressure_deadlock
{
  using namespace verona::cpp;

  struct C
  {};

  void test()
  {
    auto c1 = make_cown<C>();
    auto c2 = make_cown<C>();
    auto c3 = make_cown<C>();

    for (size_t i = 0; i < 100; i++)
      when(c1) << [](acquired_cown<C>) {};

    when(c3) << [c1](acquired_cown<C>) { when(c1) << [](acquired_cown<C>) {}; };
    when(c2) << [c2, c3](acquired_cown<C>) {
      when(c2, c3) << [](acquired_cown<C>, acquired_cown<C>) {};
    };
    when(c1) << [c1, c2](acquired_cown<C>) {
      when(c1, c2) << [](acquired_cown<C>, acquired_cown<C>) {};
    };
  }
}
