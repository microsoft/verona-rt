// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../debug/logging.h"
#include "../debug/systematic.h"
#include "../ds/forward_list.h"
#include "../region/region.h"
#include "base_noticeboard.h"
#include "core.h"
#include "schedulerthread.h"

namespace verona::rt
{
  /**
   * Shared wrapper that encapsulates the implementation of memory management
   * for Shared objects in the verona runtime.
   * This is extended to give other forms of Shared objects, such as Cowns.
   * [TODO A Notify as another subclass of Shared]
   */

  class Shared : public Object
  {
  public:
    Shared()
    {
      make_shared();
    }

  private:
    /**
     * Shared object's weak reference count.  This keeps the Shared object
     *itself alive, but not the data it can reach.  Weak reference can be
     *promoted to strong, if a strong reference still exists.
     **/
    std::atomic<size_t> weak_count{1};

  public:
    static void acquire(Object* o)
    {
      Logging::cout() << "Shared " << o << " acquire" << Logging::endl;
      assert(o->debug_is_shared());
      o->incref();
    }

    static void release(Alloc& alloc, Shared* o)
    {
      Logging::cout() << "Shared " << o << " release" << Logging::endl;
      assert(o->debug_is_shared());

      // Perform decref
      auto release_weak = false;
      bool last = o->decref_shared(release_weak);

      yield();

      if (release_weak)
      {
        o->weak_release(alloc);
        yield();
      }

      if (!last)
        return;

      // All paths from this point must release the weak count owned by the
      // strong count.

      Logging::cout() << "Cown " << o << " dealloc" << Logging::endl;

      // If last, then collect the cown body.
      o->queue_collect(alloc);
    }

    /**
     * Release a weak reference to this cown.
     **/
    void weak_release(Alloc& alloc)
    {
      Logging::cout() << "Cown " << this << " weak release" << Logging::endl;
      if (weak_count.fetch_sub(1) == 1)
      {
        yield();

        Logging::cout() << "Cown " << this << " no references left."
                        << Logging::endl;
        dealloc(alloc);
      }
    }

    void weak_acquire()
    {
      Logging::cout() << "Cown " << this << " weak acquire" << Logging::endl;
      assert(weak_count > 0);
      weak_count++;
    }

    /**
     * Gets a strong reference from a weak reference.
     *
     * Weak reference is preserved.
     *
     * Returns true is strong reference created.
     **/
    bool acquire_strong_from_weak()
    {
      bool reacquire_weak = false;
      auto result = Object::acquire_strong_from_weak(reacquire_weak);
      if (reacquire_weak)
      {
        weak_acquire();
      }
      return result;
    }

  private:
    void dealloc(Alloc& alloc)
    {
      Object::dealloc(alloc);
      yield();
    }

    /**
     * Called when strong reference count reaches one.
     * Uses thread_local state to deal with deep deallocation
     * chains by queuing recursive calls.
     **/
    void queue_collect(Alloc& alloc)
    {
      thread_local ObjectStack* work_list = nullptr;

      // If there is a already a queue, use it
      if (work_list != nullptr)
      {
        // This is a recursive call, add to queue
        // and return immediately.
        work_list->push(this);
        return;
      }

      // Make queue for recursive deallocations.
      ObjectStack current(alloc);
      work_list = &current;

      // Collect the current cown
      collect(alloc);
      yield();
      weak_release(alloc);

      // Collect recursively reachable cowns
      while (!current.empty())
      {
        auto a = (Shared*)current.pop();
        a->collect(alloc);
        yield();
        a->weak_release(alloc);
      }
      work_list = nullptr;
    }

    void collect(Alloc& alloc)
    {
#ifdef USE_SYSTEMATIC_TESTING_WEAK_NOTICEBOARDS
// TODO THINK
//      Move to finalisers for noticeboards.
//      flush_all(alloc);
#endif
      Logging::cout() << "Collecting cown " << this << Logging::endl;

      ObjectStack dummy(alloc);
      // Run finaliser before releasing our data.
      // Sub-regions handled by code below.
      finalise(nullptr, dummy);

      // Release our data.
      ObjectStack f(alloc);
      trace(f);

      while (!f.empty())
      {
        Object* o = f.pop();

        switch (o->get_class())
        {
          case RegionMD::ISO:
            Region::release(alloc, o);
            break;

          case RegionMD::RC:
          case RegionMD::SCC_PTR:
            Immutable::release(alloc, o);
            break;

          case RegionMD::SHARED:
            Logging::cout()
              << "DecRef from " << this << " to " << o << Logging::endl;
            Shared::release(alloc, (Shared*)o);
            break;

          default:
            abort();
        }
      }

      yield();

      // Now we may run our destructor.
      destructor();
    }
  };

  namespace shared
  {
    inline void release(Alloc& alloc, Object* o)
    {
      assert(o->debug_is_shared());
      Shared::release(alloc, (Shared*)o);
    }
  } // namespace cown
} // namespace verona::rt
