// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <snmalloc/snmalloc.h>

#if defined(__SANITIZE_ADDRESS__)
#  define HAS_ASAN
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define HAS_ASAN
#  endif
#endif

#if defined(HAS_ASAN)
#  include <sanitizer/asan_interface.h>
// Asan does not appear to support a __asan_update_deallocation_context
#  define VERONA_TRACK_FREE(ptr, size) __asan_poison_memory_region(ptr, size);
#  define VERONA_TRACK_ALLOC(ptr, size) \
    __asan_unpoison_memory_region(ptr, size); \
    __asan_update_allocation_context(ptr);
#  define VERONA_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#else
#  define VERONA_TRACK_FREE(ptr, size)
#  define VERONA_TRACK_ALLOC(ptr, size)
#  define VERONA_NO_SANITIZE_ADDRESS
#endif

namespace verona::rt::heap
{
#ifdef USE_REPLAY_ALLOCATOR
  class ReplayAllocator
  {
    struct Node
    {
      Node* next;
    };

    static inline std::array<Node*, 256> allocs;
    static inline std::array<size_t, 256> lengths;
    static inline snmalloc::FlagWord lock;
    static inline PRNG rng;

  public:
    static void set_seed(uint64_t seed)
    {
      rng.set_seed(seed);
    }

    VERONA_NO_SANITIZE_ADDRESS
    static void* alloc(size_t size)
    {
      auto sc = snmalloc::size_to_sizeclass_full(size);
      auto idx = sc.index();
      {
        snmalloc::FlagLock l(lock);
        if (lengths[idx] > 0)
        {
          // Reuse an element if there are at least 16, or
          // the randomisation says to.
          auto r = rng.next();
          //          std::cout << std::hex <<  r << std::endl;

          auto reuse = lengths[idx] > 16 || ((r & 0xf) == 0);
          if (reuse)
          {
            auto r = rng.next();
            Node** prev = &allocs[idx];
            for (size_t i = 0; i < r % lengths[idx]; i++)
            {
              prev = &(*prev)->next;
            }

            auto curr = *prev;
            auto next = curr->next;
            *prev = next;
            lengths[idx]--;
            VERONA_TRACK_ALLOC(curr, size);
            return curr;
          }
        }
      }

      return snmalloc::ThreadAlloc::get().alloc(size);
    }

    static void dealloc(void* ptr, size_t size)
    {
      auto sc = snmalloc::size_to_sizeclass_full(size);
      auto idx = sc.index();
      snmalloc::FlagLock l(lock);
      auto hd = reinterpret_cast<Node*>(ptr);
      hd->next = allocs[idx];
      allocs[idx] = hd;
      lengths[idx]++;
      VERONA_TRACK_FREE(ptr, size);
    }

    VERONA_NO_SANITIZE_ADDRESS
    static void flush()
    {
      for (size_t i = 0; i < allocs.size(); i++)
      {
        auto hd = allocs[i];
        size_t count = 0;
        while (hd != nullptr)
        {
          auto next = hd->next;
          snmalloc::ThreadAlloc::get().dealloc(
            hd, snmalloc::sizeclass_to_size(i));
          hd = next;
          count++;
        }
        assert(count == lengths[i]);
        lengths[i] = 0;
        allocs[i] = nullptr;
      }
    }
  };

  inline void* alloc(size_t size)
  {
    return ReplayAllocator::alloc(size);
  }

  template<size_t size>
  inline void* alloc()
  {
    return ReplayAllocator::alloc(size);
  }

  inline void* calloc(size_t size)
  {
    auto obj = ReplayAllocator::alloc(size);
    memset(obj, 0, size);
    return obj;
  }

  template<size_t size>
  inline void* calloc()
  {
    auto obj = ReplayAllocator::alloc(size);
    memset(obj, 0, size);
    return obj;
  }

  inline void dealloc(void* ptr, size_t size)
  {
    ReplayAllocator::dealloc(ptr, size);
  }

  inline void dealloc(void* ptr)
  {
    auto size = snmalloc::ThreadAlloc::get().alloc_size(ptr);
    dealloc(ptr, size);
  }

  template<size_t size>
  inline void dealloc(void* ptr)
  {
    ReplayAllocator::dealloc(ptr, size);
  }

  inline void debug_check_empty()
  {
    ReplayAllocator::flush();
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  }

  inline void set_seed(uint64_t seed)
  {
    ReplayAllocator::set_seed(seed);
  }
#else
  inline void* alloc(size_t size)
  {
    return snmalloc::ThreadAlloc::get().alloc(size);
  }

  template<size_t size>
  inline void* alloc()
  {
    return snmalloc::ThreadAlloc::get().alloc<size>();
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
    return snmalloc::ThreadAlloc::get().dealloc(ptr);
  }

  inline void dealloc(void* ptr, size_t size)
  {
    return snmalloc::ThreadAlloc::get().dealloc(ptr, size);
  }

  template<size_t size>
  inline void dealloc(void* ptr)
  {
    return snmalloc::ThreadAlloc::get().dealloc<size>(ptr);
  }

  inline void debug_check_empty()
  {
    snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  }

  inline void set_seed(uint64_t seed)
  {
    // Do nothing
  }
#endif
} // namespace verona::rt::heap