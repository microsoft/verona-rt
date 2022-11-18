// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * This test creates a scenario with two pairs of senders and receivers, where
 * the sender may overload the receiver and become muted. Another behaviour is
 * scheduled, requiring the sender from the first pair and the receiver from the
 * second pair. The backpressure system must ensure progress for the second
 * receiver, even if it is blocked on the first sender.
 */

/*
class C {}
main()
{
  var sender1 = cown.create(new C);
  var sender2 = cown.create(new C);
  var receiver1 = cown.create(new C);
  var receiver2 = cown.create(new C);

  when () { overload(sender1, receiver1) };
  when () { overload(sender2, receiver2) };
  when (sender1, receiver2) {};
}

overload(sender: C & imm, receiver: C & imm)
{
  var i: USize = 100;
  while (i > 0)
  {
    i = i - 1;
    when (sender) { when (receiver) {} }
  }
}
*/

#include "verona.h"
#include <cpp/when.h>
#include <functional>

namespace backpressure_unblock
{
  using namespace verona::cpp;

  struct Body {};

  void overload(cown_ptr<Body> sender, cown_ptr<Body> receiver)
  {
    when () << [sender, receiver]()
    {
      size_t i = 100;
      while (i > 0)
      {
        i--;
        when(sender) << [receiver](auto) { when(receiver) << [](auto) {}; };
      }
    };
  }

  void test()
  {
    auto sender1 = make_cown<Body>();
    auto sender2 = make_cown<Body>();
    auto receiver1 = make_cown<Body>();
    auto receiver2 = make_cown<Body>();

    overload(sender1, receiver1);
    overload(sender2, receiver2);
    when(sender1, receiver2) << [](auto,auto) {};
  }
}
