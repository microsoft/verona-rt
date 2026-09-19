// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../debug/logging.h"
#include "../debug/systematic.h"

#include <atomic>

namespace verona::rt
{
  namespace boc
  {
    template<class ObjectModel>
    class BehaviourCore;

    template<class ObjectModel>
    class Slot;
  }

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
     * Even numbers 2n signify n readers are reading the cown.
     * Odd numbers 2n+1 signify n readers are reading the cown and there is a
     * writer waiting.
     */
    std::atomic<size_t> count{0};

  public:
    /**
     * Add `readers` to the count. Returns true iff this is the first reader.
     *
     * The low bit may be set on entry by a delayed `try_write` from another
     * chain. The fetch_add preserves the encoding because it adds an even
     * number, and the bit is cleared by the last reader's `release_read`.
     */
    bool add_read(size_t readers = 1)
    {
      // count == 1 is a transient state in which a writer has claimed the cown
      // and no readers are present. An add_read here would be overwritten by
      // the writer clearing that state.
      assert(count.load(std::memory_order_relaxed) != 1);
      return count.fetch_add(readers * 2, std::memory_order_release) == 0;
    }

    /**
     * Remove one reader and report whether it was the last reader and whether
     * a writer is waiting.
     */
    STATUS release_read()
    {
      auto old = count.fetch_sub(2, std::memory_order_acquire);
      if (old > 3)
        return NOT_LAST;
      if (old == 2)
        return LAST_READER;

      assert(old == 3);
      Systematic::yield();
      assert(count.load(std::memory_order_relaxed) == 1);
      count.store(0, std::memory_order_relaxed);
      return LAST_READER_WAITING_WRITER;
    }

    /**
     * Attempt to begin a write.
     *
     * Returns true when no readers are active. Returns false after marking
     * that the final active reader must wake the waiting writer. Calls to this
     * function must not run concurrently with another try_write or add_read.
     */
    bool try_write()
    {
      if (count.load(std::memory_order_acquire) == 0)
        return true;

      assert(count.load(std::memory_order_relaxed) % 2 == 0);

      // Acquire observes readers registered with add_read. Release publishes
      // the waiting-writer bit to a racing release_read.
      if (count.fetch_add(1, std::memory_order_acq_rel) != 0)
        return false;

      // The readers completed between the initial load and fetch_add. Clear
      // the waiting-writer bit and proceed with the write.
      count.store(0, std::memory_order_relaxed);
      Systematic::yield();
      assert(count.load(std::memory_order_relaxed) == 0);
      return true;
    }

    size_t get_count() const
    {
      return count.load(std::memory_order_relaxed);
    }
  };

  /**
   * BoC scheduling state embedded in an object model's cown representation.
   *
   * The object model provides access to this state but does not interpret it.
   * Only the BoC protocol may manipulate these fields.
   */
  template<class ObjectModel>
  class CownSchedulerState
  {
    friend boc::BehaviourCore<ObjectModel>;
    friend boc::Slot<ObjectModel>;

    std::atomic<boc::Slot<ObjectModel>*> last_slot{nullptr};
    std::atomic<boc::BehaviourCore<ObjectModel>*> next_writer{nullptr};
    ReadRefCount read_count;

    inline friend Logging::SysLog&
    operator<<(Logging::SysLog& os, const CownSchedulerState& state)
    {
      return os << " Last slot: "
                << state.last_slot.load(std::memory_order_relaxed)
                << " Next writer: "
                << state.next_writer.load(std::memory_order_relaxed)
                << " Reader count: " << state.read_count.get_count() << " ";
    }
  };
}
