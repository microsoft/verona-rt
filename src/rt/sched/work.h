// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  /**
   * @brief A work item that can be scheduled.
   *
   * The construction of this is handled by other types that embed
   * this, and the code pointer.  See Closure below for an example.
   */
  struct Work
  {
    static constexpr auto NO_EPOCH_SET = (std::numeric_limits<uint64_t>::max)();

    union
    {
      std::atomic<Work*> next_in_queue;
      uint64_t epoch_when_popped{NO_EPOCH_SET};
    };

    void (*f)(Work*);

    constexpr Work(void (*f)(Work*)) : f(f) {}

    void run()
    {
      f(this);
    }

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
      new (t_base) T(t_param);
      return new (base) Work(&invoke<T>);
    }
  };
}