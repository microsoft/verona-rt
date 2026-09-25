// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "../memory/memory.h"

/**
 * This tests that objects on the lins stack with a refcount of 1 are attempted
 * to be deallocated only once when we release their region. release_cycles()
 * should not do duplicate inserts into the gc deallocation stack for objects
 * that get their refcount reduced to 0 from the decref_inner call in
 * release_internal. If it does, then we will end up with a double-free and a
 * crash.
 **/
namespace rc_release_lins_double_free
{
  /**
   * Use F1 (which has a destructor that decrements live_count) so we can
   * detect double-destruction via a negative live_count or a crash.
   **/
  using C = F1;

  void test_release_lins_double_free()
  {
    live_count = 0;

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

      check(live_count == 3); // o, n1, n2 are alive

      // Remove one reference to n1: n1's refcount drops from 2 to 1.
      // decref does not deallocate (RC > 0) so n1 is pushed to lins_stack.
      o->f2 = nullptr;
      decref(n1);

      // n1 is still reachable via o->f1, and is on the lins_stack.
      check(live_count == 3); // nothing deallocated yet
    }

    // region_release triggers release_internal. If we do not make sure to mark
    // objects that have their refcounts reduced to 0 from decref_inner in
    // release_internal, we will end up with release_cycles adding it to the gc
    // deallocation stack again, resulting in a double-free/crash.
    region_release(o);

    // If we get here without crashing, the fix is working.
    // All three non-iso objects should have been destructed exactly once.
    check(live_count == 0);
  }

  void run_test()
  {
    test_release_lins_double_free();
  }
}