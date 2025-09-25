// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  class ThreadState
  {
  public:
    // ThreadState counters.
    struct StateCounters
    {
      size_t active_threads{0};

      constexpr StateCounters() = default;
    };

  private:
    StateCounters internal_state;

  public:
    constexpr ThreadState() = default;

    void init(size_t threads)
    {
      internal_state.active_threads = threads;
    }

    size_t get_active_threads()
    {
      return internal_state.active_threads;
    }

    void dec_active_threads()
    {
      internal_state.active_threads--;
    }

    void inc_active_threads()
    {
      internal_state.active_threads++;
    }
  };
} // namespace verona::rt
