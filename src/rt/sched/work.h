// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "epoch.h"

#include <atomic>
#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  using namespace snmalloc;

  /**
   * @brief A work item that can be scheduled.
   *
   * The construction of this is handled by other types that embed
   * this, and the code pointer.  See Closure below for an example.
   */
  struct Work
  {
    // This references the next element in the queue.
    std::atomic<Work*> next_in_queue{nullptr};

    // The function to execute this work item. It is supplied with a self
    // pointer and is responsible for all casting and memory management.
    void (*f)(Work*);

    constexpr Work(void (*f)(Work*)) : f(f) {}

    // Helper to run the item.
    void run()
    {
      f(this);
    }
  };

  /**
   * Builds a work item from a C++ type, T.
   *
   * The internal layout is done manually to ensure we
   * aren't doing anything UB.
   */
  class Closure
  {
    /**
     * This function performs the type conversion based on the
     * layout of the types from make.
     */
    template<typename T>
    static void invoke(Work* w)
    {
      T* t = snmalloc::pointer_offset<T>(w, sizeof(Work));
      bool dealloc = (*t)(w);
      if (dealloc)
      {
        t->~T();
        heap::dealloc(w);
      }
    }

  public:
    /**
     * @brief Creates a closure that will run the `run` method of the object
     * The pointer it returns is owning.
     *
     * t_param expected to be a bool returning thunk. If it returns true, then
     * the `Work` will be deallocated.  If it returns false, then the `Work`
     * will not be deallocated, and the thunk is free to reschedule itself.
     */
    template<typename T>
    static Work* make(T&& t_param)
    {
      void* base = heap::alloc<sizeof(Work) + sizeof(T)>();
      T* t_base = snmalloc::pointer_offset<T>(base, sizeof(Work));
      new (t_base) T(std::forward<T>(t_param));
      return new (base) Work(&invoke<T>);
    }
  };
}
