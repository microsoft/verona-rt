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
    enum STATUS
    {
      LAST_READER,
      LAST_READER_WAITING_WRITER,
      NOT_LAST
    };

  private:
    /**
     * Even numbers 2n, signify n readers are reading the cown.
     * Odd numbers 2n+1, signify n readers are reading the cown, and there is a
     * writer waiting.
     */
    std::atomic<size_t> count{0};

  public:
    // true means first reader is added, false otherwise
    bool add_read(int readers = 1)
    {
      // Once a writer is waiting, no new readers can be added.
      assert(count % 2 == 0);
      return count.fetch_add(readers * 2, std::memory_order_release) == 0;
    }

    // Returns whether this is the last reader, and if there is a writer
    // waiting.
    STATUS release_read()
    {
      auto old = count.fetch_sub(2, std::memory_order_acquire);
      if (old > 3)
        return NOT_LAST;
      if (old == 2)
        return LAST_READER;

      assert(old == 3);
      Systematic::yield();
      assert(count.load() == 1);
      count.store(0, std::memory_order_relaxed);
      return LAST_READER_WAITING_WRITER;
    }

    // This function should be not be called in parellel with itself, or
    // add_read.
    //
    // The function is used to check if it is okay to proceed with a write, or
    // if the last reader should initiate this write. The function returns
    // * True means that there are no readers currently accessing
    // * False means that there are readers, but the last reader is guaranteed
    // to see LAST_READER_WAITING_WRITER.
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
      count.store(0, std::memory_order_release);
      Systematic::yield();
      assert(count.load() == 0);
      return true;
    }

    size_t get_count()
    {
      return count.load(std::memory_order_acquire);
    }
  };

  struct Slot;
  struct BehaviourCore;

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

    /**
     * MCS Queue having both readers and writers
     */
    std::atomic<Slot*> last_slot{nullptr};

    /**
     * Next writer in the queue
     */
    std::atomic<BehaviourCore*> next_writer{nullptr};

    /*
     * Cown's read ref count.
     * Bottom bit is used to signal a waiting write.
     * Remaining bits are the count.
     */
    ReadRefCount read_ref_count;

  public:
    inline friend Logging::SysLog& operator<<(Logging::SysLog& os, Cown& c)
    {
      return os << " Cown: " << &c
                << " Last slot: " << c.last_slot.load(std::memory_order_relaxed)
                << " Next writer: "
                << c.next_writer.load(std::memory_order_relaxed)
                << " Reader count: " << c.read_ref_count.get_count() << " ";
    }

#ifdef USE_SYSTEMATIC_TESTING_WEAK_NOTICEBOARDS
    std::vector<BaseNoticeboard*> noticeboards;

    void flush_all()
    {
      for (auto b : noticeboards)
      {
        b->flush_all();
      }
    }

    void flush_some()
    {
      for (auto b : noticeboards)
      {
        b->flush_some();
      }
    }

    void register_noticeboard(BaseNoticeboard* nb)
    {
      noticeboards.push_back(nb);
    }

#endif
  };
} // namespace verona::rt
