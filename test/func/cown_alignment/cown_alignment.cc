// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cpp/when.h>
#include <cstdint>
#include <debug/harness.h>

using namespace verona::cpp;

std::atomic<size_t> aligned_destructors = 0;

template<size_t Alignment>
struct alignas(Alignment) AlignedValue
{
  size_t first;
  size_t second;

  AlignedValue(size_t first, size_t second) : first(first), second(second) {}

  AlignedValue(const AlignedValue&) = delete;
  AlignedValue(AlignedValue&&) = delete;

  ~AlignedValue()
  {
    check(reinterpret_cast<uintptr_t>(this) % Alignment == 0);
    aligned_destructors.fetch_add(1, std::memory_order_relaxed);
  }
};

template<size_t Alignment>
void test_alignment()
{
  auto aligned = make_cown<AlignedValue<Alignment>>(12, 34);

  when(aligned) << [](acquired_cown<AlignedValue<Alignment>> value) {
    check(reinterpret_cast<uintptr_t>(&*value) % Alignment == 0);
    check(value->first == 12);
    check(value->second == 34);
  };

  aligned.clear();
}

void test_alignments()
{
  test_alignment<32>();
  test_alignment<64>();
  test_alignment<128>();
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  check(aligned_destructors.load(std::memory_order_relaxed) == 0);
  harness.run(test_alignments);
  check(
    aligned_destructors.load(std::memory_order_relaxed) ==
    3 * (harness.seed_upper - harness.seed_lower));

  return 0;
}
