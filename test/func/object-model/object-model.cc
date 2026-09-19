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
  get_cown_scheduler_state(Cown& cown) noexcept
  {
    return cown.scheduler_state;
  }

  static void acquire(Cown& cown) noexcept
  {
    cown.references.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(Cown& cown) noexcept
  {
    auto previous = cown.references.fetch_sub(1, std::memory_order_relaxed);
    assert(previous > 0);
  }

  static uintptr_t get_cown_identity(const Cown& cown) noexcept
  {
    return reinterpret_cast<uintptr_t>(&cown);
  }
};

using TestBehaviour = boc::BehaviourCore<TestObjectModel>;

struct alignas(64) Body
{
  std::atomic<size_t>* execution_count;
  TestCown* expected_cown;
};

void invoke(Work* work) noexcept
{
  auto* behaviour = TestBehaviour::from_work(work);
  auto* body = behaviour->get_body<Body>();
  assert(reinterpret_cast<uintptr_t>(body) % alignof(Body) == 0);
  assert(behaviour->acquired_cown(0) == body->expected_cown);
  assert(behaviour->acquired_mode(0) == AccessMode::Write);
  body->execution_count->fetch_add(1, std::memory_order_relaxed);
  body->~Body();
  TestBehaviour::finished(work);
}

int main()
{
  auto& scheduler = Scheduler::get();
  scheduler.init(2);

  TestCown cown;
  std::atomic<size_t> execution_count{0};

  auto aborted = TestBehaviour::make(1, 0, alignof(void*), invoke);
  TestBehaviour::initialise_request(
    aborted, 0, &cown, AccessMode::Write, Ownership::Borrowed);
  TestBehaviour::abort(aborted);
  assert(aborted.behaviour == nullptr);

  auto construction =
    TestBehaviour::make(1, sizeof(Body), alignof(Body), invoke);
  TestBehaviour::initialise_request(
    construction, 0, &cown, AccessMode::Write, Ownership::Borrowed);
  new (construction.body) Body{&execution_count, &cown};

  TestBehaviour::schedule(TestBehaviour::finish_construction(construction));

  scheduler.run();

  assert(execution_count.load(std::memory_order_relaxed) == 1);
  assert(cown.references.load(std::memory_order_relaxed) == 1);
  heap::debug_check_empty();
}
