// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "../memory/memory.h"

#include <vector>

/**
 * This tests the handling of an object (n2) that is reachable from the root (o)
 * node but whose only direct reference is from an object (n1) that was pushed
 * onto the lins stack but has since been deallocated. The gc cycle check should
 * not run from the deallocated n1 as, otherwise, the logic mistakenly
 * deallocates n2 due to it not being pushed onto the jump stack when being
 * trial decrefed from n1 in the mark_red phase (as n2 only has a reference from
 * the root at this point).
 **/
namespace dealloc_lins_stack_elem
{
  using C = C1;

  void test_deallocated_lins_stack_elem()
  {
    auto* o = new (RegionType::Rc) C;
    {
      UsingRegion rc(o);

      /**
          Graph structure:
          ┌───┐
          │   ▼
          o   n1──►n2
          │   ▲
          └───┘
      **/
      auto* n1 = new C;
      auto* n2 = new C;

      o->f1 = n1;
      o->f2 = n1;
      incref(n1);
      n1->f1 = n2;

      o->f1 = nullptr; // remove a o->n1
      decref(n1); // n1 should be left with ref = 1 so added to lin stack

      o->f2 = n2; // change remaining o->n1 to o->n2
      incref(n2);
      decref(n1); // n1 should deallocate here

      check(debug_size() == 2); // n1 should be deallocated

      /**
       * If the lins stack still contains a reference to n1, it will try to take
       * the subgraph starting from it (n1 -> n2) and do a trial/mark_red pass -
       * it will temporarily mark each node red and decrement its references
       * starting from n1. So n1's reference to n2 will decrement n2 to 0 and
       * the pass stops there as n2 doesn't reference anything. (No subgraph
       * nodes remain with a ref > 0 after this pass so nothing gets pushed to
       * jump_stack). It now does a scan to see if the root of the subgraph (n1)
       * remains with a ref > 0. It does not, and so it tries to restore
       * anything on the jump_stack (there is nothing) and so it deallocates n1
       * and n2. This bug can appear with the current logic if we don't remove
       * n1 from the lins stack when deallocating it.
       **/
      region_collect();
      // The correct behaviour should result in that n2 does not deallocate.
      check(debug_size() == 2);
    }
    region_release(o);
  }

  void run_test()
  {
    test_deallocated_lins_stack_elem();
  }
}