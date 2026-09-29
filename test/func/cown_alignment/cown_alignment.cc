// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cpp/when.h>
#include <cstdint>
#include <debug/harness.h>

using namespace verona::cpp;

std::atomic<size_t> aligned_destructors = 0;

struct alignas(64) AlignedValue
{
  size_t first;
  size_t second;

  AlignedValue(size_t first, size_t second) : first(first), second(second) {}

  AlignedValue(const AlignedValue&) = delete;
  AlignedValue(AlignedValue&&) = delete;

  ~AlignedValue()
  {
    check(reinterpret_cast<uintptr_t>(this) % alignof(AlignedValue) == 0);
    aligned_destructors.fetch_add(1, std::memory_order_relaxed);
  }
};

struct OrdinaryValue
{
  size_t value;

  OrdinaryValue(size_t value) : value(value) {}
};

void test_alignment()
{
  auto aligned = make_cown<AlignedValue>(12, 34);

  when(aligned) << [](acquired_cown<AlignedValue> value) {
    check(reinterpret_cast<uintptr_t>(&*value) % alignof(AlignedValue) == 0);
    check(value->first == 12);
    check(value->second == 34);
  };

  when(read(aligned)) << [](acquired_cown<const AlignedValue> value) {
    check(reinterpret_cast<uintptr_t>(&*value) % alignof(AlignedValue) == 0);
    check(value->first == 12);
    check(value->second == 34);
  };

  auto ordinary = make_cown<OrdinaryValue>(56);
  when(ordinary) <<
    [](acquired_cown<OrdinaryValue> value) { check(value->value == 56); };

  aligned.clear();
  ordinary.clear();
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  check(aligned_destructors.load(std::memory_order_relaxed) == 0);
  harness.run(test_alignment);
  check(
    aligned_destructors.load(std::memory_order_relaxed) ==
    harness.seed_upper - harness.seed_lower);

  return 0;
}
