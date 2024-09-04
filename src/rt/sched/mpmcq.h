// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "epoch.h"

namespace verona::rt
{
  /**
   * Multiple Producer Multiple Consumer Queue with steal all
   *
   * This queue forms the primary scheduler queue for each thread to
   * schedule work.
   *
   * The queue has two ends.
   *
   *   - the back end can be used by multiple thread using
   *     `enqueue` to add elements to the queue in a FIFO way wrt to `dequeue`.
   *   - the front end can be used by multiple threads to `dequeue` and
   * `dequeue_all` elements.  It is possible for dequeue to not see elements
   * added by enqueue and spuriously return nullptr.
   *
   * The empty representation of the queue has the back pointing at the front.
   * So that moving from empty to non-empty will not require branching.
   *
   * The queue uses an intrusive list in the elements of the queue.  (For
   * Verona this is the Work items).
   */
  template<class T>
  class MPMCQ
  {
  private:
    using NextPtr = std::atomic<T*>;

    std::atomic<NextPtr*> back{&front};

    // Multi-threaded end of the "queue" requires ABA protection.
    // Used for work stealing and posting new work from another thread.
    NextPtr front{nullptr};

    // Common function that is used to make the queue appear empty to any other
    // dequeue or dequeue_all operations.
    T* acquire_front()
    {
      Systematic::yield();

      // Nothing in the queue
      if (front.load(std::memory_order_relaxed) == nullptr)
      {
        return nullptr;
      }

      Systematic::yield();

      // Remove head element.  This is like locking the queue for other
      // removals.
      return front.exchange(nullptr, std::memory_order_acquire);
    }

  public:
    struct Segment
    {
      T* start;
      NextPtr* end;

      Segment(T* s, NextPtr* e) : start(s), end(e) {}

      // In place removes the first element from the segment.
      // Returns nullptr if the first element cannot be removed.
      // This can be for three reasons:
      // 1. The segment is empty
      // 2. The segment has a single element.
      // 3. The segment has link has not been completed.
      T* take_one()
      {
        auto n = start;
        if (n == nullptr)
        {
          return nullptr;
        }

        Systematic::yield();

        auto next = n->next_in_queue.load(std::memory_order_acquire);
        if (next == nullptr)
        {
          return nullptr;
        }

        start = next;
        return n;
      }
    };

    explicit MPMCQ() {}

    /**
     * Enqueue a node, this is not linearisable with respect
     * to dequeue.  That is a dequeue may not see this enqueue
     * once we return, due to other enqueues that have not
     * completed.
     */
    void enqueue_segment(Segment ls)
    {
      Systematic::yield();

      ls.end->store(nullptr, std::memory_order_relaxed);

      Systematic::yield();

      auto b = back.exchange(ls.end, std::memory_order_seq_cst);

      Systematic::yield();

      // The element we are writing into must have made its next pointer null
      // before exchanging into the structure, as the element cannot be removed
      // if it has a null next pointer, we know the write is safe.
      assert(b->load() == nullptr);
      b->store(ls.start, std::memory_order_release);
    }

    void enqueue(T* node)
    {
      enqueue_segment({node, &node->next_in_queue});
    }

    void enqueue_front(T* node)
    {
      // No longer support put on the back.
      enqueue_segment({node, &node->next_in_queue});
    }

    /**
     * Take an element from the queue.
     * This may spuriosly fail and surrounding code should be prepared for that.
     */
    T* dequeue()
    {
      auto old_front = acquire_front();

      Systematic::yield();

      // Queue is empty or someone else is stealing, and hence it will be empty
      if (old_front == nullptr)
      {
        return nullptr;
      }

      auto new_front = old_front->next_in_queue.load(std::memory_order_acquire);

      Systematic::yield();

      if (new_front != nullptr)
      {
        // Remove one element from the queue
        front.store(new_front, std::memory_order_release);
        return old_front;
      }

      Systematic::yield();

      // Queue contains a single element, attempt to close the queue
      auto next_ptr = &old_front->next_in_queue;
      if (back.compare_exchange_strong(
            next_ptr,
            &front,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
        return old_front;

      Systematic::yield();

      // Failed to close the queue, something is being added, try again later.
      front.store(old_front, std::memory_order_release);
      return nullptr;
    }

    /**
     * Take all elements from the queue.
     * This may spuriosly fail and surrounding code should be prepared for that.
     */
    Segment dequeue_all()
    {
      auto old_front = acquire_front();

      // Queue is empty or someone else is popping, so just return.
      if (old_front == nullptr)
      {
        return {nullptr, nullptr};
      }

      Systematic::yield();

      auto old_back = back.exchange(&front, std::memory_order_acq_rel);

      Systematic::yield();

      return {old_front, old_back};
    }

    bool is_empty()
    {
      Systematic::yield();

      return back.load(std::memory_order_acquire) == &front;
    }

    ~MPMCQ()
    {
      // Ensure that the queue is empty before destruction.
      assert(is_empty());
    }
  };
} // namespace verona::rt
