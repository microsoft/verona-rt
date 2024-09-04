// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include "../ds/wrapindex.h"
#include "mpmcq.h"
#include "work.h"

namespace verona::rt
{
  template<size_t N>
  class WorkStealingQueue
  {
    WrapIndex<N> enqueue_index{};
    WrapIndex<N> dequeue_index{};
    WrapIndex<N> steal_index{};

    MPMCQ<Work> queues[N];

    // Enqueue an entire segment onto the next enqueue queue.
    // Works in a round robin fashion.
    void enqueue(MPMCQ<Work>::Segment ls)
    {
      queues[++enqueue_index].enqueue_segment(ls);
    }

    // Take a segment and spread it across the queues
    // using a round robin strategy.
    void enqueue_spread(MPMCQ<Work>::Segment ls)
    {
      while (true)
      {
        auto n = ls.take_one();
        if (n == nullptr)
          break;
        enqueue(n);
      }
      enqueue(ls);
    }

  public:
    constexpr WorkStealingQueue() {}

    // Enqueue a single node onto the next enqueue queue.
    void enqueue(Work* work)
    {
      enqueue({work, &work->next_in_queue});
    }

    void enqueue_front(Work* work)
    {
      // TODO should this be --dequeue_index?
      queues[++enqueue_index].enqueue_front(work);
    }

    // Dequeue a single node from any of the queues.
    // Returns nullptr if no node is available.
    Work* dequeue()
    {
      // Try each queue once.
      for (size_t i = 0; i < N; ++i)
      {
        auto n = queues[++dequeue_index].dequeue();
        if (n != nullptr)
        {
          return n;
        }
      }

      return nullptr;
    }

    /**
     * Steal work from the victim.
     * Stealing has the side effect of placing work from the victim onto all the
     * queues. Returns nullptr if no work could be stolen. This may spuriously
     * return nullptr in the case where the first link in the segment has not
     * been created, and there are more than two elements.
     */
    Work* steal(WorkStealingQueue& victim)
    {
      if (&victim == this)
      {
        // Don't steal from yourself.
        // As scheduler loops around all the queues, use this to change the
        // index.
        ++steal_index;
        return nullptr;
      }

      auto ls = victim.queues[steal_index].dequeue_all();

      auto r = ls.take_one();
      if (r == nullptr)
      {
        // take_one can fail for three reasons:
        //  * fully empty
        //  * single element
        //  * first link in segment assignment has not become visible
        // Handle single element segment case
        if (ls.end == nullptr)
        {
          // Fully empty case
          return nullptr;
        }

        if (ls.start != nullptr && ls.end == &ls.start->next_in_queue)
        {
          // Single element queue.
          return ls.start;
        }
      }

      enqueue_spread(ls);
      return r;
    }

    bool is_empty()
    {
      for (size_t i = 0; i < N; ++i)
      {
        if (!queues[i].is_empty())
        {
          return false;
        }
      }

      return true;
    }
  };
} // namespace verona::rt