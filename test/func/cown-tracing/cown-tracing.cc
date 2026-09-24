// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

/**
 * Tests destruction of typed cowns that own other cowns and Verona objects.
 *
 * There is no global tracing of cowns. A cown_ptr field therefore uses normal
 * RAII ownership and is released when its containing T is destroyed. If T
 * defines trace(ObjectStack&), ActualCown<T> forwards that method so T can
 * report runtime-managed region and immutable references.
 *
 * The test covers three ownership shapes:
 *
 * 1. A cown owns another cown through cown_ptr. The owner schedules work on
 *    the child and is then destroyed. The queued behaviour must keep the child
 *    alive until that work runs.
 *
 * 2. A cown owns both a child cown and a trace region. The region owns an
 *    immutable object. The child is released by C++ destruction, while the
 *    region and immutable are released through T::trace.
 *
 * 3. A cown owns a chain of trace and arena subregions as well as a child
 *    cown. This checks that tracing the outer region finds and destroys each
 *    subregion without affecting the independently RAII-managed child.
 *
 * Each case checks both useful work and exact destructor counts. The harness
 * additionally checks that no allocation remains after scheduler shutdown.
 */
namespace
{
  size_t cores;
  std::atomic<size_t> finished_threads{0};

  std::atomic<size_t> direct_owner_destroyed{0};
  std::atomic<size_t> direct_child_destroyed{0};
  std::atomic<size_t> direct_child_work{0};

  std::atomic<size_t> mixed_owner_destroyed{0};
  std::atomic<size_t> mixed_region_destroyed{0};
  std::atomic<size_t> mixed_child_destroyed{0};
  std::atomic<size_t> mixed_immutable_destroyed{0};
  std::atomic<size_t> mixed_child_work{0};

  std::atomic<size_t> nested_owner_destroyed{0};
  std::atomic<size_t> nested_region_destroyed{0};
  std::atomic<size_t> nested_child_destroyed{0};
  std::atomic<size_t> nested_child_work{0};

  struct DirectChild
  {
    ~DirectChild()
    {
      direct_child_destroyed++;
    }
  };

  struct DirectOwner
  {
    cown_ptr<DirectChild> child;

    DirectOwner(cown_ptr<DirectChild> child) : child(std::move(child)) {}

    ~DirectOwner()
    {
      direct_owner_destroyed++;
    }
  };

  struct MixedChild
  {
    ~MixedChild()
    {
      mixed_child_destroyed++;
    }
  };

  struct MixedImmutable : public V<MixedImmutable>
  {
    void trace(ObjectStack&) const {}

    ~MixedImmutable()
    {
      mixed_immutable_destroyed++;
    }
  };

  struct MixedRegion : public V<MixedRegion>
  {
    MixedImmutable* immutable = nullptr;

    void trace(ObjectStack& fields) const
    {
      if (immutable != nullptr)
        fields.push(immutable);
    }

    ~MixedRegion()
    {
      mixed_region_destroyed++;
    }
  };

  struct MixedOwner
  {
    cown_ptr<MixedChild> child;
    MixedRegion* region;

    MixedOwner(cown_ptr<MixedChild> child, MixedRegion* region)
    : child(std::move(child)), region(region)
    {}

    void trace(ObjectStack& fields) const
    {
      fields.push(region);
    }

    ~MixedOwner()
    {
      mixed_owner_destroyed++;
    }
  };

  struct NestedChild
  {
    ~NestedChild()
    {
      nested_child_destroyed++;
    }
  };

  struct NestedRegion : public V<NestedRegion>
  {
    NestedRegion* subregion = nullptr;

    void trace(ObjectStack& fields) const
    {
      if (subregion != nullptr)
        fields.push(subregion);
    }

    void finaliser(Object* region, ObjectStack& subregions)
    {
      Object::add_sub_region(subregion, region, subregions);
    }

    ~NestedRegion()
    {
      nested_region_destroyed++;
    }
  };

  struct NestedOwner
  {
    cown_ptr<NestedChild> child;
    NestedRegion* region;

    NestedOwner(cown_ptr<NestedChild> child, NestedRegion* region)
    : child(std::move(child)), region(region)
    {}

    void trace(ObjectStack& fields) const
    {
      fields.push(region);
    }

    ~NestedOwner()
    {
      nested_owner_destroyed++;
    }
  };

  void reset_counts()
  {
    direct_owner_destroyed = 0;
    direct_child_destroyed = 0;
    direct_child_work = 0;

    mixed_owner_destroyed = 0;
    mixed_region_destroyed = 0;
    mixed_child_destroyed = 0;
    mixed_immutable_destroyed = 0;
    mixed_child_work = 0;

    nested_owner_destroyed = 0;
    nested_region_destroyed = 0;
    nested_child_destroyed = 0;
    nested_child_work = 0;
  }

  void direct_cown_edge()
  {
    auto child = make_cown<DirectChild>();
    auto owner = make_cown<DirectOwner>(std::move(child));

    when(std::move(owner)) << [](acquired_cown<DirectOwner> owner) {
      auto child = owner->child;
      when(std::move(child))
        << [](acquired_cown<DirectChild>) { direct_child_work++; };
    };
  }

  void mixed_region_edges()
  {
    auto region = new (RegionType::Trace) MixedRegion;

    region->immutable = new (RegionType::Trace) MixedImmutable;
    freeze(region->immutable);
    RegionTrace::insert<YesTransfer>(region, region->immutable);

    auto child = make_cown<MixedChild>();
    auto owner = make_cown<MixedOwner>(std::move(child), region);
    when(std::move(owner)) << [](acquired_cown<MixedOwner> owner) {
      auto child = owner->child;
      when(std::move(child))
        << [](acquired_cown<MixedChild>) { mixed_child_work++; };
    };
  }

  void nested_subregions()
  {
    auto outer = new (RegionType::Trace) NestedRegion;
    outer->subregion = new (RegionType::Trace) NestedRegion;
    outer->subregion->subregion = new (RegionType::Arena) NestedRegion;

    auto child = make_cown<NestedChild>();
    auto owner = make_cown<NestedOwner>(std::move(child), outer);
    when(std::move(owner)) << [](acquired_cown<NestedOwner> owner) {
      auto child = owner->child;
      when(std::move(child))
        << [](acquired_cown<NestedChild>) { nested_child_work++; };
    };
  }

  void test_body()
  {
    reset_counts();
    direct_cown_edge();
    mixed_region_edges();
    nested_subregions();
  }

  void finish()
  {
    if (finished_threads.fetch_add(1) != cores - 1)
      return;

    check(direct_owner_destroyed == 1);
    check(direct_child_destroyed == 1);
    check(direct_child_work == 1);

    check(mixed_owner_destroyed == 1);
    check(mixed_region_destroyed == 1);
    check(mixed_child_destroyed == 1);
    check(mixed_immutable_destroyed == 1);
    check(mixed_child_work == 1);

    check(nested_owner_destroyed == 1);
    check(nested_region_destroyed == 3);
    check(nested_child_destroyed == 1);
    check(nested_child_work == 1);

    finished_threads = 0;
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);
  cores = harness.cores;
  harness.run_at_termination = finish;
  harness.run(test_body);
}
