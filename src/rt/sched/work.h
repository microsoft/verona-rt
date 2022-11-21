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
    static constexpr auto NO_EPOCH_SET = (std::numeric_limits<uint64_t>::max)();

    // Entry in the MPMC Queue of work items per scheduler.
    union
    {
      // When in the queue this references the next element
      std::atomic<Work*> next_in_queue;
      // When not in the queue this is the epoch that it was last seen in the
      // queue. This is used to allow deferred deallocation to be more
      // efficient.
      uint64_t epoch_when_popped{NO_EPOCH_SET};
    };

    // The function to execute this work item. It is supplied with a self
    // pointer and is responsible for all casting and memory management.
    void (*f)(Work*);

    constexpr Work(void (*f)(Work*)) : f(f) {}

    // Helper to run the item.
    void run()
    {
      f(this);
    }

    /**
     * Helper to perform deallocation.
     *
     * Due to complexity with the MPMCQ memory management there is an
     * epoch mechanism to prevent the underlying allocator decommitting
     * the memory and thus causing a segfault.  This correctly deallocates
     * a work item.
     *
     * It assumes the work item is the start of an underlying allocation, and
     * does not work if it is embedded not at the start of an allocation.
     */
    void dealloc()
    {
      auto& alloc = ThreadAlloc::get();
      auto epoch = epoch_when_popped;
      auto outdated = epoch == NO_EPOCH_SET || GlobalEpoch::is_outdated(epoch);
      if (outdated)
      {
        Logging::cout() << "Work " << this << " dealloc" << Logging::endl;
        alloc.dealloc(this);
      }
      else
      {
        Logging::cout() << "Work " << this << " defer dealloc" << Logging::endl;
        // There could be an ABA problem if we reuse this work as the epoch
        // has not progressed enough We delay the deallocation until the epoch
        // has progressed enough
        // TODO: We are waiting too long as this is inserting in the current
        // epoch, and not `epoch` which is all that is required.
        Epoch e(alloc);
        e.delete_in_epoch(this);
      }
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
        w->dealloc();
      }
    }

  public:
    /**
     * @brief Creates a closure that will run the `run` method of the object
     * The pointer it returns is owning.
     *
     * t_param expected to be a bool returning thunk. If it returns true, then
     * the `Work` will be deallocated (subject to epoch checks).  If it returns
     * false, then the `Work` will not be deallocated, and the thunk is free to
     * reschedule itself.
     */
    template<typename T>
    static Work* make(T&& t_param)
    {
      void* base =
        snmalloc::ThreadAlloc::get().alloc<sizeof(Work) + sizeof(T)>();
      T* t_base = snmalloc::pointer_offset<T>(base, sizeof(Work));
      new (t_base) T(std::forward<T>(t_param));
      return new (base) Work(&invoke<T>);
    }
  };
}