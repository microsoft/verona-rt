// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <boc/behaviourcore.h>
#include <sched/schedulerthread.h>

using namespace verona::rt;

struct TestObjectModel;

struct alignas(8) TestCown
{
  CownSchedulerState<TestObjectModel> scheduler_state;
  std::atomic<size_t> references{1};
};

struct TestObjectModel
{
  using Cown = TestCown;

  static CownSchedulerState<TestObjectModel>&
  get_cown_scheduler_state(Cown& cown)
  {
    return cown.scheduler_state;
  }

  static void acquire(Cown& cown)
  {
    cown.references.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(Cown& cown)
  {
    auto previous = cown.references.fetch_sub(1, std::memory_order_relaxed);
    assert(previous > 0);
  }

  static uintptr_t get_cown_identity(const Cown& cown)
  {
    return reinterpret_cast<uintptr_t>(&cown);
  }
};

using TestBehaviour = boc::BehaviourCore<TestObjectModel>;
using TestSlot = boc::Slot<TestObjectModel>;

struct Payload
{
  std::atomic<size_t>* execution_count;
};

void invoke(Work* work)
{
  auto* behaviour = TestBehaviour::from_work(work);
  auto* payload = behaviour->get_body<Payload>();
  payload->execution_count->fetch_add(1, std::memory_order_relaxed);
  payload->~Payload();
  TestBehaviour::finished(work);
}

int main()
{
  auto& scheduler = Scheduler::get();
  scheduler.init(2);

  TestCown cown;
  std::atomic<size_t> execution_count{0};

  auto* behaviour = TestBehaviour::make(1, invoke, sizeof(Payload));
  new (behaviour->get_slots()) TestSlot(&cown);
  new (behaviour->get_body<Payload>()) Payload{&execution_count};

  TestBehaviour* batch[] = {behaviour};
  TestBehaviour::schedule(batch, 1);

  scheduler.run();

  assert(execution_count.load(std::memory_order_relaxed) == 1);
  assert(cown.references.load(std::memory_order_relaxed) == 1);
  heap::debug_check_empty();
}
