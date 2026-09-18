// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <boc/behaviourcore.h>
#include <sched/schedulerthread.h>

using namespace verona::rt;

struct TestObjectModel;

struct alignas(8) TestCown
{
  CownSchedulerState<TestObjectModel> scheduler_state;
  std::atomic<size_t> references{2};
  std::atomic<bool> logically_destroyed{false};
};

struct TestObjectModel
{
  using Cown = TestCown;

  static CownSchedulerState<TestObjectModel>&
  get_cown_scheduler_state(Cown& cown)
  {
    assert(!cown.logically_destroyed.load(std::memory_order_relaxed));
    return cown.scheduler_state;
  }

  static void acquire(Cown& cown)
  {
    assert(!cown.logically_destroyed.load(std::memory_order_relaxed));
    cown.references.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(Cown& cown)
  {
    auto previous = cown.references.fetch_sub(1, std::memory_order_relaxed);
    assert(previous > 0);
    if (previous == 1)
      cown.logically_destroyed.store(true, std::memory_order_relaxed);
  }

  static uintptr_t get_cown_identity(const Cown& cown)
  {
    return reinterpret_cast<uintptr_t>(&cown);
  }
};

using TestBehaviour = boc::BehaviourCore<TestObjectModel>;
using TestSlot = boc::Slot<TestObjectModel>;

void invoke(Work* work)
{
  TestBehaviour::finished(work);
}

int main()
{
  auto& scheduler = Scheduler::get();
  scheduler.init(2);

  TestCown cown;
  auto* behaviour = TestBehaviour::make(2, invoke, 0);
  auto* slots = behaviour->get_slots();

  new (&slots[0]) TestSlot(&cown);
  slots[0].set_move();

  new (&slots[1]) TestSlot(&cown);
  slots[1].set_read_only();
  slots[1].set_move();

  TestBehaviour* batch[] = {behaviour};
  TestBehaviour::schedule(batch, 1);

  scheduler.run();

  assert(cown.references.load(std::memory_order_relaxed) == 0);
  assert(cown.logically_destroyed.load(std::memory_order_relaxed));
  heap::debug_check_empty();
}
