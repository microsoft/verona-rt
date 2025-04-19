// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <snmalloc/snmalloc.h>

namespace verona::rt::heap
{
#ifdef SNMALLOC_PASS_THROUGH
  inline void* aligned_alloc_snmalloc_size(size_t size)
  {
    auto align = snmalloc::natural_alignment(size);
    auto asize = snmalloc::bits::align_up(size, align);
    return aligned_alloc(align, asize);
  }

  inline void* alloc(size_t size)
  {
    return aligned_alloc_snmalloc_size(size);
  }

  template<size_t size>
  inline void* alloc()
  {
    return aligned_alloc_snmalloc_size(size);
  }

  inline void* calloc(size_t size)
  {
    auto p = aligned_alloc_snmalloc_size(size);
    memset(p, 0, size);
    return p;
  }

  template<size_t size>
  inline void* calloc()
  {
    auto p = aligned_alloc_snmalloc_size(size);
    memset(p, 0, size);
    return p;
  }

  inline void dealloc(void* ptr)
  {
    free(ptr);
  }

  inline void dealloc(void* ptr, size_t size)
  {
    free(ptr);
  }

  template<size_t size>
  inline void dealloc(void* ptr)
  {
    free(ptr);
  }

  inline void debug_check_empty() {}
#else
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
#endif
} // namespace verona::rt::heap
