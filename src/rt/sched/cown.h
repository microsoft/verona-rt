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
#include "shared.h"

#include <algorithm>
#include <vector>

namespace verona::rt
{
  using namespace snmalloc;
  class Cown;
  using Scheduler = ThreadPool<SchedulerThread>;

  /**
   * A cown, or concurrent owner, encapsulates a set of resources that may be
   * accessed by a single (scheduler) thread at a time when writing, or
   * accessed by multiple (scheduler) threads at a time when reading.
   * A cown can only be in one of the following states:
   *   1. Unscheduled
   *   2. Scheduled, in the queue of a single scheduler thread
   *   3. Running with read/write access on a single scheduler thread, and not
   * in the queue of any scheduler thread
   *   4. Running with read access on one or more scheduler threads, and may
   * also be in the queue of one other scheduler thread
   *
   * Once a cown is running, it executes a batch of multi-message behaviours.
   * Each message may either acquire the running cown for participation in a
   * future behaviour, or execute the behaviour if it is the last cown to be
   * acquired.
   * If the running cown is acquired for writing for a future behaviour, it will
   * be descheduled until that behaviour has completed. If the running cown is
   * acquired for reading for a future behaviour, it will not be descheduled. If
   * the running cown is acquired for reading _and_ executing on this thread,
   * the cown will be rescheduled to be picked up by another thread. (it might
   * later return to this thread if this is the last thread to use the cown in
   * read more before a write).
   */

  struct ReadRefCount
  {
    std::atomic<size_t> count{0};

    void add_read()
    {
      count.fetch_add(2);
    }

    // true means last reader and writer is waiting, false otherwise
    bool release_read()
    {
      if (count.fetch_sub(2) == 3)
      {
        Systematic::yield();
        assert(count.load() == 1);
        count.store(0, std::memory_order_relaxed);
        return true;
      }
      return false;
    }

    bool try_write()
    {
      if (count.load(std::memory_order_acquire) == 0)
        return true;

      // Mark a pending write
      if (count.fetch_add(1) != 0)
        return false;

      // if in the time between reading and writing the ref count, it
      // became zero, we can now process the write, so clear the flag
      // and continue
      count.fetch_sub(1);
      Systematic::yield();
      assert(count.load() == 0);
      return true;
    }
  };

  struct Slot;

  class Cown : public Shared
  {
  public:
    Cown() {}

  private:
    friend Core;
    friend Slot;
    template<typename T>
    friend class Promise;
    friend struct BehaviourCore;

    template<typename T>
    friend class Noticeboard;

    std::atomic<Slot*> last_slot{nullptr};

    /*
     * Cown's read ref count.
     * Bottom bit is used to signal a waiting write.
     * Remaining bits are the count.
     */
    ReadRefCount read_ref_count;

  public:
#ifdef USE_SYSTEMATIC_TESTING_WEAK_NOTICEBOARDS
    std::vector<BaseNoticeboard*> noticeboards;

    void flush_all(Alloc& alloc)
    {
      for (auto b : noticeboards)
      {
        b->flush_all(alloc);
      }
    }

    void flush_some(Alloc& alloc)
    {
      for (auto b : noticeboards)
      {
        b->flush_some(alloc);
      }
    }

    void register_noticeboard(BaseNoticeboard* nb)
    {
      noticeboards.push_back(nb);
    }

#endif

    // void mark_notify()
    // {
    //   if (queue.mark_notify())
    //   {
    //     Cown::acquire(this);
    //     schedule();
    //   }
    //   yield();
    // }

  private:
    // void cown_notified()
    // {
    //   // This is not a message make sure we know that.
    //   // TODO: Back pressure.  This means that a notification that sends to
    //   // an overloaded cown will not mute this cown.  We could set up a fake
    //   // message structure, or alter how the backpressure system determines
    //   // which is/are the currently active cowns.
    //   Scheduler::local()->message_body = nullptr;
    //   notified();
    // }

  public:
    // bool release_early()
    // {
    //   auto* body = Scheduler::local()->message_body;
    //   auto* senders = body->get_requests_array();
    //   const size_t senders_count = body->count;
    //   Alloc& alloc = ThreadAlloc::get();

    //   /*
    //    * Avoid releasing the last cown because it breaks the current
    //    * code structure
    //    */
    //   if (this == senders[senders_count - 1].cown())
    //     return false;

    //   for (size_t s = 0; s < senders_count; s++)
    //   {
    //     if (senders[s].cown() != this)
    //       continue;

    //     if (
    //       !senders[s].is_read() ||
    //       senders[s].cown()->read_ref_count.release_read())
    //     {
    //       senders[s].cown()->schedule();
    //     }
    //     else
    //     {
    //       Cown::release(alloc, senders[s].cown());
    //     }

    //     senders[s] = Request();

    //     break;
    //   }

    //   return true;
    // }
  };
} // namespace verona::rt
