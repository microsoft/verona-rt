// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../ds/stackarray.h"
#include "../object/object.h"
#include "cown.h"

#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  using namespace snmalloc;

  class Request
  {
    Cown* _cown;

    static constexpr uintptr_t READ_FLAG = 0x1;
    static constexpr uintptr_t MOVE_FLAG = 0x2;

    Request(Cown* cown) : _cown(cown) {}

  public:
    Request() : _cown(nullptr) {}

    Cown* cown()
    {
      return (Cown*)((uintptr_t)_cown & ~(READ_FLAG | MOVE_FLAG));
    }

    bool is_read()
    {
      return ((uintptr_t)_cown & READ_FLAG);
    }

    bool is_move()
    {
      return ((uintptr_t)_cown & MOVE_FLAG);
    }

    void mark_move()
    {
      _cown = (Cown*)((uintptr_t)_cown | MOVE_FLAG);
    }

    static Request write(Cown* cown)
    {
      return Request(cown);
    }

    static Request read(Cown* cown)
    {
      return Request((Cown*)((uintptr_t)cown | READ_FLAG));
    }
  };

  struct BehaviourCore;

  struct Slot
  {
    Cown* cown;
    /**
     * Possible values before scheduling to communicate memory management
     * options:
     *   0 - Borrow
     *   1 - Move
     *
     * Possible vales after scheduling:
     *   0 - Wait
     *   1 - Ready
     *   Behaviour* - Next write
     *
     * TODO Read-only When we extend to read-only we will need the following
     * additional state
     *   Slot* - Next Read
     *   2 - Ready and Read available
     */
    std::atomic<uintptr_t> status;

    Slot(Cown* cown) : cown(cown), status(0) {}

    bool is_ready()
    {
      return status.load(std::memory_order_acquire) == 1;
    }

    void set_move()
    {
      status.store(1, std::memory_order_relaxed);
    }

    void reset_status()
    {
      status.store(0, std::memory_order_relaxed);
    }

    void set_ready()
    {
      status.store(1, std::memory_order_release);
    }

    bool is_wait()
    {
      return status.load(std::memory_order_relaxed) == 0;
    }

    bool is_behaviour()
    {
      return status.load(std::memory_order_relaxed) > 1;
    }

    BehaviourCore* get_behaviour()
    {
      return (BehaviourCore*)status.load(std::memory_order_acquire);
    }

    void set_behaviour(BehaviourCore* b)
    {
      status.store((uintptr_t)b, std::memory_order_release);
    }

    void release();

    void reset()
    {
      status.store(0, std::memory_order_release);
    }
  };

  /**
   * @brief This class implements the core logic for the `when` construct in the
   * runtime.
   *
   * It is based on the using the class MCS Queue Lock to build a dag of
   * behaviours.  Each cown can be seen as a lock, and each behaviour is
   * like a wait node in the queues for multiple cowns.
   *
   * Unlike, the MCS Queue Lock, we do not spin waiting for the behaviour
   * to be actionable, but instead the behaviour carries the code, that
   * can be scheduled when it has no predecessors in the dag.
   *
   * MCS Queue Lock paper:
   *   J. M. Mellor-Crummey and M. L. Scott. Algorithms for
   *   scalable synchronization on shared-memory
   *   multiprocessors. ACM TOCS, 9(1):21–65, Feb. 1991
   *
   * The class does not instiate the behaviour fully which is done by
   * the `Behaviour` class. This allows for code reuse with a notification
   * mechanism.
   */
  struct BehaviourCore
  {
    std::atomic<size_t> exec_count_down;
    size_t count;

    /**
     * @brief Construct a new Behaviour object
     *
     * @param count - number of cowns to acquire
     *
     * Note that exec_count_down is initialised to count + 1. This is because
     * we use the additional count to protect against the behaviour being
     * completely executed, before we have finished setting Ready on all the
     * slots. Two phase locking needs to complete before we can execute the
     * behaviour.
     */
    BehaviourCore(size_t count) : exec_count_down(count + 1), count(count) {}

    Work* as_work()
    {
      return pointer_offset_signed<Work>(
        this, -static_cast<ptrdiff_t>(sizeof(Work)));
    }

    /**
     * @brief Given a pointer to a work object converts it to a
     * BehaviourCore pointer.
     *
     * This is inherently unsafe, and should only be used when it is known the
     * work object was constructed using `make`.
     */
    static BehaviourCore* from_work(Work* w)
    {
      return pointer_offset<BehaviourCore>(w, sizeof(Work));
    }

    /**
     * Remove `n` from the exec_count_down.
     *
     * Returns true if this call makes the count_down_zero
     */
    void resolve(size_t n = 1)
    {
      Logging::cout() << "Behaviour::resolve " << n << " for behaviour " << this
                      << Logging::endl;
      // Note that we don't actually perform the last decrement as it is not
      // required.
      if (
        (exec_count_down.load(std::memory_order_acquire) == n) ||
        (exec_count_down.fetch_sub(n) == n))
        Scheduler::schedule(as_work());
    }

    // TODO: When C++ 20 move to span.
    Slot* get_slots()
    {
      return pointer_offset<Slot>(this, sizeof(BehaviourCore));
    }

    template<typename T = void>
    T* get_body()
    {
      Slot* slots = pointer_offset<Slot>(this, sizeof(BehaviourCore));
      return pointer_offset<T>(slots, sizeof(Slot) * count);
    }

    /**
     * @brief Constructs a behaviour.  Leaves space for the closure.
     *
     * @param count - Number of slots to allocate, i.e. how many cowns to wait
     * for.
     * @param f - The function to execute once all the behaviours dependencies
     * are ready.
     * @param payload - The size of the payload to allocate.
     * @return BehaviourCore* - the pointer to the behaviour object.
     */
    static BehaviourCore* make(size_t count, void (*f)(Work*), size_t payload)
    {
      // Manual memory layout of the behaviour structure.
      //   | Work | Behaviour | Slot ... Slot | Body |
      size_t size =
        sizeof(Work) + sizeof(BehaviourCore) + (sizeof(Slot) * count) + payload;
      void* base = ThreadAlloc::get().alloc(size);

      Work* work = new (base) Work(f);
      void* base_behaviour = from_work(work);
      BehaviourCore* behaviour = new (base_behaviour) BehaviourCore(count);

      // These assertions are basically checking that we won't break any
      // alignment assumptions on Be.  If we add some actual alignment, then
      // this can be improved.
      static_assert(
        sizeof(Slot) % sizeof(void*) == 0,
        "Slot size must be a multiple of pointer size");
      static_assert(
        sizeof(BehaviourCore) % sizeof(void*) == 0,
        "Behaviour size must be a multiple of pointer size");
      static_assert(
        sizeof(Work) % sizeof(void*) == 0,
        "Work size must be a multiple of pointer size");

      return behaviour;
    }

    /**
     * @brief Schedule a behaviour for execution.
     *
     * @param bodies  The behaviours to schedule.
     *
     * @param body_count The number of behaviours to schedule.
     *
     * @note
     *
     * *** Single behaviour scheduling ***
     *
     * To correctly implement the happens before order, we need to ensure that
     * one when cannot overtake another:
     *
     *   when (a, b, d) { B1 } || when (a, c, d) { B2 }
     *
     * Where we assume the underlying acquisition order is alphabetical.
     *
     * Let us assume B1 exchanges on `a` first, then we need to ensure that
     * B2 cannot acquire `d` before B1 as this would lead to a cycle.
     *
     * To achieve this we effectively do two phase locking.
     *
     * The first (acquire) phase performs exchange on each cown in same
     * global assumed order.  It can only move onto the next cown once the
     * previous behaviour on that cown specifies it has completed its scheduling
     * by marking itself ready, `set_ready`.
     *
     * The second (release) phase is simply making each slot ready, so that
     * subsequent behaviours can continue scheduling.
     *
     * Returning to our example earlier, if B1 exchanges on `a` first, then
     * B2 will have to wait for B1 to perform all its exchanges, and mark the
     * appropriate slot ready in phase two. Hence, it is not possible for any
     * number of behaviours to form a cycle.
     *
     *
     * Invariant: If the cown is part of a chain, then the scheduler holds an RC
     * on the cown. This means the first behaviour to access a cown will need to
     * perform an acquire. When the execution of a chain completes, then the
     * scheduler will release the RC.
     *
     * *** Extension to Many ***
     *
     * This code additional can schedule a group of behaviours atomically.
     *
     *   when (a) { B1 } + when (b) { B2 } + when (a, b) { B3 }
     *
     * This will cause the three behaviours to be scheduled in a single atomic
     * step using the two phase commit.  This means that no other behaviours can
     * access a between B1 and B3, and no other behaviours can access b between
     * B2 and B3.
     *
     * This extension is implemented by building a mapping from each request
     * to the sorted order of.  In this case that produces
     *
     *  0 -> 0, a
     *  1 -> 1, b
     *  2 -> 2, a
     *  3 -> 2, b
     *
     * which gets sorted to
     *
     *  0 -> 0, a
     *  1 -> 2, a
     *  2 -> 1, b
     *  3 -> 2, b
     *
     * We then link the (0,a) |-> (2,a), and enqueue the segment atomically onto
     * cown a, and then link (1,b) |-> (2,b) and enqueue the segment atomically
     * onto cown b.
     *
     * By enqueuing segments we ensure nothing can get in between the
     * behaviours.
     *
     * *** Duplicate Cowns ***
     *
     * The final complication the code must deal with is duplicate cowns.
     *
     * when (a, a) { B1 }
     *
     * To handle this we need to detect the duplicate cowns, and mark the slot
     * as not needing a successor.  This is done by setting the cown pointer to
     * nullptr.
     *
     * Consider the following complex example
     *
     * when (a) { ... } + when (a,a) { ... } + when (a) { ... }
     *
     * This will produce the following mapping
     *
     * 0 -> 0, a
     * 1 -> 1, a (0)
     * 2 -> 1, a (1)
     * 3 -> 2, a
     *
     * This is sorted already, so we can just link the segments
     *
     * (0,a) |-> (1, a (0)) |-> (2, a)
     *
     * and mark (a (1)) as not having a successor.
     */
    static void schedule_many(BehaviourCore** bodies, size_t body_count)
    {
      Logging::cout() << "BehaviourCore::schedule_many" << body_count
                      << Logging::endl;

      size_t count = 0;
      for (size_t i = 0; i < body_count; i++)
        count += bodies[i]->count;

      // Execution count - we will remove at least
      // one from the execution count on finishing phase 2 of the
      // 2PL. This ensures that the behaviour cannot be
      // deallocated until we finish phase 2.
      StackArray<size_t> ec(body_count);
      for (size_t i = 0; i < body_count; i++)
        ec[i] = 1;

      // Need to sort the cown requests across the co-scheduled collection of
      // cowns We first construct an array that represents pairs of behaviour
      // number and slot pointer. Note: Really want a dynamically sized stack
      // allocation here.
      StackArray<std::tuple<size_t, Slot*>> indexes(count);
      size_t idx = 0;
      for (size_t i = 0; i < body_count; i++)
      {
        auto slots = bodies[i]->get_slots();
        for (size_t j = 0; j < bodies[i]->count; j++)
        {
          std::get<0>(indexes[idx]) = i;
          std::get<1>(indexes[idx]) = &slots[j];
          idx++;
        }
      }

      // Sort the indexing array so we make the requests in the correct order
      // across the whole set of behaviours.  A consistent order is required to
      // avoid deadlock.
      // We sort first by cown, and then by behaviour number.
      // These means overlaps will be in a sequence in the array in the correct
      // order with respect to the order of the group of behaviours.
      auto compare = [](
                       const std::tuple<size_t, Slot*> i,
                       const std::tuple<size_t, Slot*> j) {
#ifdef USE_SYSTEMATIC_TESTING
        return std::get<1>(i)->cown->id() == std::get<1>(j)->cown->id() ?
          std::get<0>(i) < std::get<0>(j) :
          std::get<1>(i)->cown->id() < std::get<1>(j)->cown->id();
#else
        return std::get<1>(i)->cown == std::get<1>(j)->cown ?
          std::get<0>(i) < std::get<0>(j) :
          std::get<1>(i)->cown < std::get<1>(j)->cown;
#endif
      };
      if (count > 1)
        std::sort(indexes.get(), indexes.get() + count, compare);

      // First phase - Acquire phase.
      size_t i = 0;
      while (i < count)
      {
        auto cown = std::get<1>(indexes[i])->cown;
        auto body = bodies[std::get<0>(indexes[i])];
        auto last_slot = std::get<1>(indexes[i]);
        auto first_body = body;
        size_t first_chain_index = i;

        // The number of RCs provided for the current cown by the when.
        // I.e. how many moves of cown_refs there were.
        size_t transfer_count = last_slot->status;

        // Detect duplicates for this cown.
        // This is required in two cases:
        //  * overlaps with multiple behaviours; and
        //  * overlaps within a single behaviour.
        while (((++i) < count) && (cown == std::get<1>(indexes[i])->cown))
        {
          // Check if the caller passed an RC and add to the total.
          transfer_count += std::get<1>(indexes[i])->status;

          // If the body is the same, then we have an overlap within a single
          // behaviour.
          auto body_next = bodies[std::get<0>(indexes[i])];
          if (body_next == body)
          {
            Logging::cout() << "Duplicate cown: " << cown << " for behaviour "
                            << body << Logging::endl;
            // We need to reduce the execution count by one, as we can't wait
            // for ourselves.
            ec[std::get<0>(indexes[i])]++;

            // We need to mark the slot as not having a cown associated to it.
            std::get<1>(indexes[i])->cown = nullptr;
            continue;
          }
          body = body_next;

          // Extend the chain of behaviours linking on this behaviour
          last_slot->set_behaviour(body);
          last_slot = std::get<1>(indexes[i]);
        }

        last_slot->reset_status();

        auto prev =
          cown->last_slot.exchange(last_slot, std::memory_order_acq_rel);

        // set_behaviour to the first_slot
        yield();
        if (prev == nullptr)
        {
          // this is wrong - should only do it for the last one
          Logging::cout() << "Acquired cown: " << cown << " for behaviour "
                          << body << Logging::endl;

          ec[std::get<0>(indexes[first_chain_index])]++;

          yield();

          if (transfer_count)
          {
            Logging::cout() << "Releasing transferred count " << transfer_count
                            << Logging::endl;
            // Release transfer_count - 1 times, we needed one as we woke up the
            // cown, but the rest were not required.
            for (int j = 0; j < transfer_count - 1; j++)
              Cown::release(ThreadAlloc::get(), cown);
          }
          else
          {
            Logging::cout()
              << "Acquiring reference count on cown: " << cown << Logging::endl;
            // We didn't have any RCs passed in, so we need to acquire one.
            Cown::acquire(cown);
          }
          continue;
        }

        Logging::cout() << "Waiting for cown: " << cown << " from slot " << prev
                        << " for behaviour " << body << Logging::endl;

        yield();
        while (prev->is_wait())
        {
          // Wait for the previous behaviour to finish adding to first phase.
          Aal::pause();
          Systematic::yield_until([prev]() { return !prev->is_wait(); });
        }

        Logging::cout() << "Releasing transferred count " << transfer_count
                        << Logging::endl;
        // Release as many times as indicated
        for (int j = 0; j < transfer_count; j++)
          Cown::release(ThreadAlloc::get(), cown);

        yield();
        prev->set_behaviour(first_body);
        yield();
      }

      // Second phase - Release phase.
      for (size_t i = 0; i < body_count; i++)
      {
        Logging::cout() << "Release phase for behaviour " << bodies[i]
                        << Logging::endl;
      }
      for (size_t i = 0; i < count; i++)
      {
        yield();
        auto slot = std::get<1>(indexes[i]);
        Logging::cout() << "Setting slot " << slot << " to ready"
                        << Logging::endl;
        if (slot->is_wait())
          slot->set_ready();
      }

      for (size_t i = 0; i < body_count; i++)
      {
        yield();
        bodies[i]->resolve(ec[i]);
      }
    }

    /**
     * @brief Release all slots in the behaviour.
     *
     * This is should be called when the behaviour has executed.
     */
    void release_all()
    {
      auto slots = get_slots();
      // Behaviour is done, we can resolve successors.
      for (size_t i = 0; i < count; i++)
      {
        slots[i].release();
      }
    }

    /**
     * Reset the behaviour to look like it has never been scheduled.
     */
    void reset()
    {
      // Reset status on slots.
      for (size_t i = 0; i < count; i++)
      {
        get_slots()[i].reset();
      }

      exec_count_down = count + 1;
    }
  };

  inline void Slot::release()
  {
    assert(!is_wait());

    // This slot represents a duplicate cown, so we can ignore releasing it.
    if (cown == nullptr)
      return;

    if (is_ready())
    {
      yield();
      auto slot_addr = this;
      // Attempt to CAS cown to null.
      if (cown->last_slot.compare_exchange_strong(
            slot_addr, nullptr, std::memory_order_acq_rel))
      {
        yield();
        Logging::cout() << "No more work for cown " << cown << Logging::endl;
        // Success, no successor, release scheduler threads reference count.
        shared::release(ThreadAlloc::get(), cown);
        return;
      }

      yield();

      // If we failed, then the another thread is extending the chain
      while (is_ready())
      {
        Systematic::yield_until([this]() { return !is_ready(); });
        Aal::pause();
      }
    }

    assert(is_behaviour());
    // Wake up the next behaviour.
    yield();
    get_behaviour()->resolve();
    yield();
  }
} // namespace verona::rt
