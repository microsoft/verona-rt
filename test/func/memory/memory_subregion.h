// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "memory.h"

namespace memory_subregion
{
  template<RegionType region_type>
  struct O : public V<O<region_type>, region_type>
  {
    O<RegionType::Trace>* f1 = nullptr;
    O<RegionType::Arena>* f2 = nullptr;

    void trace(ObjectStack& st) const
    {
      if (f1 != nullptr)
        st.push(f1);

      if (f2 != nullptr)
        st.push(f2);
    }

    void finaliser(Object* region, ObjectStack& sub_regions)
    {
      Object::add_sub_region(f1, region, sub_regions);
      Object::add_sub_region(f2, region, sub_regions);
    }
  };

  /**
   * Tests allocating (and then releasing) a single region, which has pointers
   * to singleton subregions.
   **/
  template<RegionType region_type>
  void test_subregion_singleton()
  {
    using C = C3<region_type>;
    using F = F3<region_type>;

    if constexpr (region_type == RegionType::Rc)
    {
      auto* r = new F;
      auto* r1 = new C;
      auto* r2 = new F;
      auto* r3 = new F;
      auto* r4 = new F;

      {
        UsingRegion rc(r);
        r->c1 = new (alloc, r) C;
        r->c1->f2 = new (alloc, r) F;

        r->c1->f2->c1 = r1;
        r->f1 = r2;
        r->f2 = new (alloc, r) F;
        r->f2->f1 = r3;
        r->f2->f1->f1 = r4;

        Region::release(r);
      }
    }
    else
    {
      auto* r = new F;
      r->c1 = new (alloc, r) C;
      r->c1->f2 = new (alloc, r) F;
      r->c1->f2->c1 = new C; // new subregion
      r->f1 = new F; // new subregion
      r->f2 = new (alloc, r) F;
      r->f2->f1 = new F; // new subregion
      r->f2->f1->f1 = new F; // new subregion

      alloc_in_region<C, F>(alloc, r); // unreachable

      Region::release(r);
    }
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
    check(live_count == 0);
  }

  /**
   * A basic test for subregions within a region.
   **/
  template<RegionType region_type>
  void test_subregion_basic()
  {
    using C = C3<region_type>;
    using F = F3<region_type>;

    if constexpr (region_type == RegionType::Rc)
    {
      // Start with a single region.
      auto* r = new F;
      {
        UsingRegion rc(r);
        r->c1 = new (alloc, r) C;
        r->f1 = new (alloc, r) F;
        r->c2 = new (alloc, r) C;
        r->c1->c1 = new (alloc, r) C;
        r->c1->f1 = new (alloc, r) F;
      }

      // Now create some subregions.
      auto* r1 = new F;
      {
        UsingRegion rc(r1);
        r1->c1 = new (alloc, r1) C;
        r1->c1->c1 = new (alloc, r1) C;
        r1->c1->c1->c1 = new (alloc, r1) C;
      }

      auto* r2 = new C;
      {
        UsingRegion rc(r2);
        r2->c1 = new (alloc, r2) C;
        r2->f1 = new (alloc, r2) F;
        r2->f1->c1 = new (alloc, r2) C;
        r2->f1->f1 = new (alloc, r2) F;
      }

      auto* r3 = new F;
      {
        UsingRegion rc(r3);
        r3->f1 = new (alloc, r3) F;
      }

      /* // Link the subregions together. */
      r->f2 = r1;
      r->f1->c1 = r2;
      r2->f1->f1->f1 = r3;

      {
        UsingRegion rc(r);
        Region::release(r);
      }
    }
    else
    {
      // Start with a single region.
      auto* r = new F;
      r->c1 = new (alloc, r) C;
      r->f1 = new (alloc, r) F;
      r->c2 = new (alloc, r) C;
      r->c1->c1 = new (alloc, r) C;
      r->c1->f1 = new (alloc, r) F;
      alloc_in_region<C, F>(alloc, r); // unreachable

      // Now create some subregions.
      auto* r1 = new F;
      r1->c1 = new (alloc, r1) C;
      r1->c1->c1 = new (alloc, r1) C;
      r1->c1->c1->c1 = new (alloc, r1) C;
      alloc_in_region<F, F, F>(alloc, r1); // unreachable

      auto* r2 = new C;
      r2->c1 = new (alloc, r2) C;
      r2->f1 = new (alloc, r2) F;
      r2->f1->c1 = new (alloc, r2) C;
      r2->f1->f1 = new (alloc, r2) F;
      alloc_in_region<C, C>(alloc, r2); // unreachable

      auto* r3 = new F;
      r3->f1 = new (alloc, r3) F;
      alloc_in_region<F, F>(alloc, r3); // unreachable

      // Link the subregions together.
      r->f2 = r1;
      r->f1->c1 = r2;
      r2->f1->f1->f1 = r3;

      Region::release(r);
    }
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
    check(live_count == 0);
  }

  /**
   * Test subregions of different types.
   **/
  void test_subregion_mix()
  {
    using OTrace = O<RegionType::Trace>;
    using OArena = O<RegionType::Arena>;

    // Start with a single region.
    auto* r = new OTrace;
    r->f1 = new (alloc, r) OTrace;
    r->f1->f1 = new (alloc, r) OTrace;
    alloc_in_region<OTrace, OTrace>(alloc, r); // unreachable

    auto* garbage = new (alloc, r) OTrace; // unreachable within r
    garbage->f1 = new OTrace; // new subregion
    garbage->f2 = new OArena; // new subregion

    // Now create some subregions.
    auto* r1 = new OTrace;
    r1->f1 = new (alloc, r1) OTrace;
    alloc_in_region<OTrace, OTrace>(alloc, r1); // unreachable

    auto* r2 = new OArena;
    r2->f2 = new (alloc, r2) OArena;
    r2->f2->f2 = new (alloc, r2) OArena;
    r2->f2->f2->f2 = new (alloc, r2) OArena;
    alloc_in_region<OArena, OArena>(alloc, r2); // unreachable

    auto* r3 = new OTrace;
    alloc_in_region<OTrace, OTrace>(alloc, r3); // unreachable

    // Connect the subregions.
    r->f1->f1->f1 = r1;
    r->f2 = r2;
    r2->f1 = r3;

    // Count items in each subregion.
    check(Region::debug_size(r) == 6);
    check(Region::debug_size(r1) == 4);
    check(Region::debug_size(r2) == 6);
    check(Region::debug_size(r3) == 3);

    // GC r3, then r, but not r1.
    RegionTrace::gc(alloc, r3);
    RegionTrace::gc(alloc, r);

    // Check the sizes again.
    check(Region::debug_size(r) == 3);
    check(Region::debug_size(r1) == 4);
    check(Region::debug_size(r2) == 6);
    check(Region::debug_size(r3) == 1);

    Region::release(r);
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  }

  /**
   * Tests involving subregions and swapping roots.
   **/
  template<RegionType region_type>
  void test_subregion_swap_root()
  {
    using RegionClass = typename RegionType_to_class<region_type>::T;
    using C = C3<region_type>;
    using F = F3<region_type>;

    // Subregion hanging off the entry object, but then we swap root.
    {
      auto* oroot = new F;
      auto* nroot = new (alloc, oroot) C;
      oroot->c1 = nroot;
      oroot->f1 = new (alloc, oroot) F;

      auto* r2 = new F;
      r2->c1 = new (alloc, r2) C;
      r2->c2 = new (alloc, r2) C;

      oroot->f2 = r2;

      RegionClass::swap_root(oroot, nroot);

      if constexpr (region_type == RegionType::Trace)
      {
        // After the swap, we have some unreachable objects.
        check(Region::debug_size(nroot) == 3);
        RegionTrace::gc(alloc, nroot);
        check(Region::debug_size(nroot) == 1);
      }

      Region::release(nroot);
      snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
    }

    // After swapping the root, the subregion now hangs off the entry object.
    {
      auto* oroot = new F;
      auto* nroot = new (alloc, oroot) F;
      oroot->f1 = new (alloc, oroot) F;
      oroot->f2 = nroot;

      auto* r2 = new F;
      r2->c1 = new (alloc, r2) C;
      r2->c2 = new (alloc, r2) C;

      nroot->f1 = r2;

      RegionClass::swap_root(oroot, nroot);

      if constexpr (region_type == RegionType::Trace)
      {
        // After the swap, we have some unreachable objects.
        check(Region::debug_size(nroot) == 3);
        RegionTrace::gc(alloc, nroot);
        check(Region::debug_size(nroot) == 1);
      }

      Region::release(nroot);
      snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
    }

    // Swap root for the subregion, and patch up parent region's pointer.
    {
      auto* r = new F;
      r->c1 = new (alloc, r) C;
      r->f1 = new (alloc, r) F;

      auto* oroot = new F;
      auto* nroot = new (alloc, oroot) F;
      oroot->f1 = nroot;
      oroot->c2 = new (alloc, oroot) C;

      r->f2 = oroot;

      RegionClass::swap_root(oroot, nroot);

      // Need to patch up r->f2's pointer.
      r->f2 = nroot;

      if constexpr (region_type == RegionType::Trace)
      {
        // After the swap, we have some unreachable objects.
        check(Region::debug_size(nroot) == 3);
        RegionTrace::gc(alloc, nroot);
        check(Region::debug_size(nroot) == 1);
      }

      Region::release(r);
      snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
    }
  }

  template<RegionType region_type>
  void test_subregion_merge()
  {
    using RegionClass = typename RegionType_to_class<region_type>::T;
    using C = C3<region_type>;
    using F = F3<region_type>;

    // Create the first region, with some unreachable objects.
    auto* r1 = new C;
    r1->c1 = new (alloc, r1) C;
    r1->f1 = new (alloc, r1) F;
    alloc_in_region<F, F>(alloc, r1); // unreachable

    // Create the second region.
    auto* r2 = new F;
    r2->c1 = new (alloc, r2) C;
    r2->c1->c1 = new (alloc, r2) C;
    r2->c1->c1->c1 = new (alloc, r2) C;
    r2->c1->c1->f1 = new (alloc, r2) F;
    alloc_in_region<C, C>(alloc, r2); // unreachable

    // Now merge them.
    RegionClass::merge(alloc, r1, r2);

    // Make sure r1 can reach r2's object graph.
    r1->f2 = r2;

    // Let's allocate some more.
    alloc_in_region<F, F>(alloc, r1);

    // Run GC.
    if constexpr (region_type == RegionType::Trace)
    {
      check(Region::debug_size(r1) == 14);
      RegionTrace::gc(alloc, r1);
      check(Region::debug_size(r1) == 8);
    }

    // Let's break a link to create some more garbage, then run GC again.
    r2->c1->c1 = nullptr;
    if constexpr (region_type == RegionType::Trace)
    {
      RegionTrace::gc(alloc, r1);
      check(Region::debug_size(r1) == 5);
    }

    Region::release(r1);
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  }

  template<RegionType region_type>
  void test_subregion_deep()
  {
    using F = F3<region_type>;

    // Create the first region, with some unreachable objects.
    auto* r1 = new F;
    auto curr = r1;
    std::cout << "Build long region chain." << std::endl;
    for (size_t i = 0; i < 1 << 20; i++)
    {
      auto n = new F;
      curr->f1 = n;
      curr = n;
    }
    std::cout << "Dealloc long region chain." << std::endl;

    Region::release(r1);
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
    std::cout << "Dealloced long region chain." << std::endl;
  }

  void run_test()
  {
    test_subregion_singleton<RegionType::Trace>();
    test_subregion_singleton<RegionType::Arena>();
    test_subregion_singleton<RegionType::Rc>();

    test_subregion_basic<RegionType::Trace>();
    test_subregion_basic<RegionType::Arena>();
    test_subregion_basic<RegionType::Rc>();

    test_subregion_mix();

    test_subregion_swap_root<RegionType::Trace>();
    test_subregion_swap_root<RegionType::Arena>();

    test_subregion_deep<RegionType::Trace>();

    test_subregion_merge<RegionType::Trace>();
    test_subregion_merge<RegionType::Arena>();
  }
}