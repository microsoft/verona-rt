#pragma once

#include <snmalloc/snmalloc.h>

namespace verona::rt::heap
{
  inline void* alloc(size_t size)
  {
    return heap::alloc(size);
  }

  template<size_t size>
  inline void* alloc()
  {
    return heap::alloc<size>();
  }

  inline void* calloc(size_t size)
  {
    return snmalloc::ThreadAlloc::get().alloc<snmalloc::YesZero>(size);
  }

  template<size_t size>
  inline void* calloc()
  {
    return snmalloc::ThreadAlloc::get().alloc<size, snmalloc::YesZero>();
  }


  inline void dealloc(void* ptr)
  {
    return heap::dealloc(ptr);
  }

  inline void dealloc(void* ptr, size_t size)
  {
    return heap::dealloc(ptr, size);
  }

  template <size_t size> inline void dealloc(void* ptr)
  {
    return heap::dealloc<size>(ptr);
  }

  inline void debug_check_empty()
  {
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  }
} // namespace verona::rt::heap