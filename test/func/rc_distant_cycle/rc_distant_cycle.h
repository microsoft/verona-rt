// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "../memory/memory.h"

#include <vector>

/**
 * This tests if a cycle (n2->n3->n2) with just one external reference that is
 * reachable from the root (n1) is correctly deallocated when we decref (and
 * indirectly deallocate) that external reference.
 **/
namespace rc_distant_cycle
{
  using C = C1;

  void test_distant_cycle()
  {
    auto* o = new (RegionType::Rc) C;
    {
      UsingRegion rc(o);

      /**
          Graph structure:
                    ┌────┐
                    │    ▼
          o──►n1──►n2   n3
                    ▲    │
                    └────┘
      **/
      auto* n1 = new C;
      auto* n2 = new C;
      auto* n3 = new C;

      o->f1 = n1;
      n1->f1 = n2;
      n2->f1 = n3;
      n3->f1 = n2;

      incref(n2);

      o->f1 = nullptr; // remove o->n1
      decref(n1); // decref n1, it should deallocate

      check(debug_size() == 3); // only n1 should be deallocated so far

      // Trigger cycle collection to try to collect n2->n3->n2 cycle
      region_collect();

      /**
       * n1, n2 and n3 should be deallocated by now. A bug of debug_size == 3
       * can arise if we don't add n2 to the lins stack when its ref count stays
       * > 0 after n1 is deallocated and its reference is removed.
       **/
      check(debug_size() == 1);
    }
    region_release(o);
  }

  void run_test()
  {
    test_distant_cycle();
  }
}