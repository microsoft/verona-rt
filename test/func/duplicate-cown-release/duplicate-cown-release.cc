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
  get_cown_scheduler_state(Cown& cown) noexcept
  {
    assert(!cown.logically_destroyed.load(std::memory_order_relaxed));
    return cown.scheduler_state;
  }

  static void acquire(Cown& cown) noexcept
  {
    assert(!cown.logically_destroyed.load(std::memory_order_relaxed));
    cown.references.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(Cown& cown) noexcept
  {
    auto previous = cown.references.fetch_sub(1, std::memory_order_relaxed);
    assert(previous > 0);
    if (previous == 1)
      cown.logically_destroyed.store(true, std::memory_order_relaxed);
  }

  static uintptr_t get_cown_identity(const Cown& cown) noexcept
  {
    return reinterpret_cast<uintptr_t>(&cown);
  }
};

using TestBehaviour = boc::BehaviourCore<TestObjectModel>;

void invoke(Work* work) noexcept
{
  TestBehaviour::finished(work);
}

int main()
{
  auto& scheduler = Scheduler::get();
  scheduler.init(2);

  TestCown cown;
  auto construction = TestBehaviour::make(2, 0, alignof(void*), invoke);
  TestBehaviour::initialise_request(
    construction, 0, &cown, AccessMode::Write, Ownership::Transferred);
  TestBehaviour::initialise_request(
    construction, 1, &cown, AccessMode::Read, Ownership::Transferred);

  TestBehaviour::schedule(TestBehaviour::finish_construction(construction));

  scheduler.run();

  assert(cown.references.load(std::memory_order_relaxed) == 0);
  assert(cown.logically_destroyed.load(std::memory_order_relaxed));
  heap::debug_check_empty();
}
