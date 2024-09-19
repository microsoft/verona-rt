// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <test/measuretime.h>
#include <test/opt.h>
#include <verona.h>

using namespace snmalloc;
using namespace verona::rt;

void test_epoch()
{
  // Used to prevent malloc from being optimised away.
  static void* old = nullptr;

  constexpr int count = 10000000;
  constexpr int size = 48;
  void* special = heap::alloc(size);
  void* obj = nullptr;

  std::cout << "Start epoch test" << std::endl;

  {
    MeasureTime m;
    m << "with_epoch   ";
    for (int n = 0; n < count; n++)
    {
      Epoch e;
      obj = heap::alloc(size);
      e.delete_in_epoch(obj);
    }

    Epoch::flush();
  }

  {
    MeasureTime m;
    m << "without_epoch";
    for (int n = 0; n < count; n++)
    {
      obj = heap::alloc(size);
      old = obj;
      heap::dealloc(obj, size);
    }
  }

  {
    MeasureTime m;
    m << "template_no_e";
    for (int n = 0; n < count; n++)
    {
      obj = heap::alloc<size>();
      old = obj;
      heap::dealloc<size>(obj);
    }
  }

  heap::dealloc(special);
  heap::debug_check_empty();
  (void)old;
}

int main(int, char**)
{
  test_epoch();
  return 0;
}
