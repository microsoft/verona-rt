// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <snmalloc/snmalloc.h>

namespace verona::rt::heap
{
  inline void* alloc(size_t size)
  {
    return snmalloc::alloc(size);
  }

  template<size_t size>
  inline void* alloc()
  {
    return snmalloc::alloc<size>();
  }

  inline void* calloc(size_t size)
  {
    return snmalloc::alloc<snmalloc::YesZero>(size);
  }

  template<size_t size>
  inline void* calloc()
  {
    return snmalloc::alloc<size, snmalloc::YesZero>();
  }

  inline void dealloc(void* ptr)
  {
    return snmalloc::dealloc(ptr);
  }

  inline void dealloc(void* ptr, size_t size)
  {
    return snmalloc::dealloc(ptr, size);
  }

  template<size_t size>
  inline void dealloc(void* ptr)
  {
    return snmalloc::dealloc<size>(ptr);
  }

  inline void debug_check_empty()
  {
    snmalloc::debug_check_empty();
  }
} // namespace verona::rt::heap