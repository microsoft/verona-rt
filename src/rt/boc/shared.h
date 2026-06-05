// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../debug/logging.h"
#include "../debug/systematic.h"
#include "../ds/forward_list.h"
#include "../region/region.h"
#include "base_noticeboard.h"

namespace verona::rt
{
  /**
   * Shared wrapper that encapsulates the implementation of memory management
   * for Shared objects in the verona runtime.
   * This is extended to give other forms of Shared objects, such as Cowns.
   * [TODO A Notify as another subclass of Shared]
   *
   * --- Atomic ordering invariant ---
   *
   * Reference-count operations follow the classic shared_ptr pattern:
   *
   *   - acquire (incref / weak_acquire): relaxed fetch_add.  The caller
   *     already holds a reference, so they already have whatever data HB
   *     they need; the count itself does not need to acquire anything.
   *
   *   - release (decref / decref_shared / weak_release): release fetch_sub.
   *     This publishes any writes the calling thread made to the object
   *     body through its reference, so the eventual destroyer sees them.
   *
   *   - last-decrementer: acquires all the releases.  For the strong count
   *     this is the compare_exchange_strong to FINISHED_RC in
   *     Object::decref_shared (acquire).  For Object::decref it is an
   *     explicit acquire fence.  For weak_release it is also an explicit
   *     acquire fence.
   *
   * Most data writes to a cown body happen under the slot/behaviour 2PL
   * protocol (see boc/behaviourcore.h::Slot, Cown::last_slot) which already
   * provides release/acquire ordering between successive behaviours.  The
   * release on the refcount is the defensive edge that covers the
   * destruction path itself.
   *
   * Exception: acquire_strong_from_weak uses release (not relaxed) on its
   * fetch_add to rc so that decref_shared's CAS failure path (acquire)
   * can see the promoter's preceding weak_acquire.  See object.h for the
   * cross-variable synchronisation argument.
   *
   * See "Wait-free weak reference counting" (doi:10.1145/3591195.3595271)
   * for the formal argument for the two-phase strong-count close.
   * https://dl.acm.org/doi/10.1145/3591195.3595271
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

    static void release(Shared* o)
    {
      Logging::cout() << "Shared " << o << " release" << Logging::endl;
      assert(o->debug_is_shared());

      // Perform decref
      auto release_weak = false;
      bool last = o->decref_shared(release_weak);

      yield();

      if (release_weak)
      {
        o->weak_release();
        yield();
      }

      if (!last)
        return;

      // All paths from this point must release the weak count owned by the
      // strong count.

      Logging::cout() << "Cown " << o << " dealloc" << Logging::endl;

      // If last, then collect the cown body.
      o->queue_collect();
    }

    /**
     * Release a weak reference to this cown.
     **/
    void weak_release()
    {
      Logging::cout() << "Cown " << this << " weak release" << Logging::endl;
      // Release: publish any writes through this weak ref to the eventual
      // deallocator.
      if (weak_count.fetch_sub(1, std::memory_order_release) == 1)
      {
        // Last decrementer: acquire all the releases from the other
        // weak_release calls on this object before calling dealloc().
        std::atomic_thread_fence(std::memory_order_acquire);

        yield();

        Logging::cout() << "Cown " << this << " no references left."
                        << Logging::endl;
        dealloc();
      }
    }

    void weak_acquire()
    {
      Logging::cout() << "Cown " << this << " weak acquire" << Logging::endl;
      assert(weak_count.load(std::memory_order_relaxed) > 0);
      // Relaxed: pure arithmetic.  The caller already holds a live ref.
      // In the promotion path (acquire_strong_from_weak), this runs
      // *after* rc.fetch_add(release).  The rc release does not publish
      // this later increment, but safety is structural: the promoter's
      // pre-existing weak ref keeps weak_count >= 1, so a racing
      // decref_shared weak_release cannot drive it to zero.
      weak_count.fetch_add(1, std::memory_order_relaxed);
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
    void dealloc()
    {
      Object::dealloc();
      yield();
    }

    /**
     * Called when strong reference count reaches one.
     * Uses thread_local state to deal with deep deallocation
     * chains by queuing recursive calls.
     **/
    void queue_collect()
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
      ObjectStack current;
      work_list = &current;

      // Collect the current cown
      collect();
      yield();
      weak_release();

      // Collect recursively reachable cowns
      while (!current.empty())
      {
        auto a = (Shared*)current.pop();
        a->collect();
        yield();
        a->weak_release();
      }
      work_list = nullptr;
    }

    void collect()
    {
#ifdef USE_SYSTEMATIC_TESTING_WEAK_NOTICEBOARDS
// TODO THINK
//      Move to finalisers for noticeboards.
//      flush_all(alloc);
#endif
      Logging::cout() << "Collecting cown " << this << Logging::endl;

      ObjectStack dummy;
      // Run finaliser before releasing our data.
      // Sub-regions handled by code below.
      finalise(nullptr, dummy);

      // Release our data.
      ObjectStack f;
      trace(f);

      while (!f.empty())
      {
        Object* o = f.pop();

        switch (o->get_class())
        {
          case RegionMD::ISO:
            Region::release(o);
            break;

          case RegionMD::RC:
          case RegionMD::SCC_PTR:
            Immutable::release(o);
            break;

          case RegionMD::SHARED:
            Logging::cout()
              << "DecRef from " << this << " to " << o << Logging::endl;
            Shared::release((Shared*)o);
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
    inline void release(Object* o)
    {
      assert(o->debug_is_shared());
      Shared::release((Shared*)o);
    }
  } // namespace cown
} // namespace verona::rt
