// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <iostream>
#include <sched/work.h>
#include <snmalloc/snmalloc.h>

using namespace verona::rt;

void check_order(std::string str, size_t index)
{
  static size_t current = 0;
  if (index != current)
  {
    std::cout << "Out of order execution: Expected " << current << " got "
              << index << std::endl;
    abort();
  }
  current++;
  std::cout << str << std::endl;
}

int main()
{
  auto w = Closure::make([](Work* w) {
    check_order("Work - Run", 1);
    snmalloc::ThreadAlloc::get().dealloc(w);
    check_order("Work - Done", 2);
  });

  check_order("Constructed", 0);
  w->run();
  check_order("Run done", 3);

  return 0;
}
