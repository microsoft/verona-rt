// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#ifndef USE_REPLAY_ALLOCATOR
#  define USE_REPLAY_ALLOCATOR
#endif

#include <debug/harness.h>
#include <test/opt.h>

/**
 * Tests heap replay functionality with systematic testing.
 *
 * We count each time alloc returns the value just freed, this should
 * be possible, but not guaranteed.
 **/

size_t count = 0;

void test_replay()
{
  size_t size = 16;
  void* p = heap::alloc(size);
  uintptr_t p_addr = (uintptr_t)p;
  heap::dealloc(p, size);

  void* p2 = heap::alloc(size);
  uintptr_t p2_addr = (uintptr_t)p2;
  heap::dealloc(p2, size);

  if (p_addr == p2_addr)
  {
    std::cout << "*" << std::flush;
    count++;
  }
  else
  {
    std::cout << "." << std::flush;
  }
}

int main(int argc, char** argv)
{
  auto t = Aal::tick();
  size_t repeats = 1000;
  for (size_t i = 0; i < repeats; i++)
  {
    heap::set_seed(i + t);
    test_replay();
    heap::debug_check_empty();
    if (i % 64 == 0)
    {
      std::cout << std::endl;
    }
  }

  std::cout << std::endl << "count: " << count << std::endl;

  // We should have at least one replay
  check(count != 0);
  // We should not have all replays
  check(count != repeats);

  return 0;
}
