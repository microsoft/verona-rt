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

    static constexpr uintptr_t READ_FLAG = 1;

    Request(Cown* cown) : _cown(cown) {}

  public:
    Request() : _cown(nullptr) {}

    Cown* cown()
    {
      return (Cown*)((uintptr_t)_cown & ~READ_FLAG);
    }

    bool is_read()
    {
      return ((uintptr_t)_cown & READ_FLAG);
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
     * Possible vales:
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
     * @tparam transfer - NoTransfer or YesTransfer - NoTransfer means
     * any cowns that are scheduled will need a reference count added to them.
     * YesTransfer means that the caller is transfering a reference count to
     * each of the cowns, so the schedule should remove a count if it is not
     * required.
     * @param body  The behaviour to schedule.
     *
     * @note
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
     */
    template<TransferOwnership transfer = NoTransfer>
    static void schedule(BehaviourCore* body)
    {
      Logging::cout() << "BehaviourCore::schedule " << body << Logging::endl;
      auto count = body->count;
      auto slots = body->get_slots();

      // Really want a dynamically sized stack allocation here.
      StackArray<size_t> indexes(count);
      for (size_t i = 0; i < count; i++)
        indexes[i] = i;

      auto compare = [slots](const size_t i, const size_t j) {
#ifdef USE_SYSTEMATIC_TESTING
        return slots[i].cown->id() > slots[j].cown->id();
#else
        return slots[i].cown > slots[j].cown;
#endif
      };

      if (count > 1)
        std::sort(indexes.get(), indexes.get() + count, compare);

      // Execution count - we will remove at least
      // one from the execution count on finishing phase 2 of the
      // 2PL. This ensures that the behaviour cannot be
      // deallocated until we finish phase 2.
      size_t ec = 1;

      // First phase - Acquire phase.
      for (size_t i = 0; i < count; i++)
      {
        auto cown = slots[indexes[i]].cown;
        auto prev = cown->last_slot.exchange(
          &slots[indexes[i]], std::memory_order_acq_rel);

        yield();

        if (prev == nullptr)
        {
          Logging::cout() << "Acquired cown: " << cown << " for behaviour "
                          << body << Logging::endl;
          ec++;
          if (transfer == NoTransfer)
          {
            yield();
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

        if (transfer == YesTransfer)
        {
          Cown::release(ThreadAlloc::get(), cown);
        }

        yield();
        prev->set_behaviour(body);
        yield();
      }

      // Second phase - Release phase.
      Logging::cout() << "Release phase for behaviour " << body
                      << Logging::endl;
      for (size_t i = 0; i < count; i++)
      {
        yield();
        Logging::cout() << "Setting slot " << &slots[indexes[i]] << " to ready"
                        << Logging::endl;
        slots[i].set_ready();
      }

      yield();
      body->resolve(ec);
    }

    template<TransferOwnership transfer = NoTransfer>
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

      // Really want a dynamically sized stack allocation here.
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
      auto compare = [](
                       const std::tuple<size_t, Slot*> i,
                       const std::tuple<size_t, Slot*> j) {
#ifdef USE_SYSTEMATIC_TESTING
        return std::get<1>(i)->cown->id() > std::get<1>(j)->cown->id();
#else
        return std::get<1>(i)->cown > std::get<1>(j)->cown;
#endif
      };

      if (count > 1)
        std::sort(indexes.get(), indexes.get() + count, compare);

      // First phase - Acquire phase.
      for (size_t i = 0; i < count; i++)
      {
        auto cown = std::get<1>(indexes[i])->cown;
        auto body = bodies[std::get<0>(indexes[i])];
        auto prev = cown->last_slot.exchange(
          std::get<1>(indexes[i]), std::memory_order_acq_rel);
        auto* ec_ptr = &ec[std::get<0>(indexes[i])];

        yield();

        if (prev == nullptr)
        {
          Logging::cout() << "Acquired cown: " << cown << " for behaviour "
                          << body << Logging::endl;
          (*ec_ptr)++;
          if (transfer == NoTransfer)
          {
            yield();
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

        if (transfer == YesTransfer)
        {
          Cown::release(ThreadAlloc::get(), cown);
        }

        yield();
        prev->set_behaviour(body);
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
