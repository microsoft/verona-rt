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

  inline Logging::SysLog& operator<<(Logging::SysLog&, BehaviourCore&);

  struct Slot
  {
  private:
    /**
     * Cown required by this behaviour
     *
     * Bit 0 - Possible values:
     *          0 - Wait (2PL in acquire phase going on)
     *          1 - Ready (2PL in acquire phase done)
     *
     * Bit 1 - Possible values:
     *          0 - Current slot Writer
     *          1 - Current slot Reader
     *
     * Remaining bits - Cown pointer
     *
     * Assumption - Cowns are allocated at 4 byte boundary. Last 2 bits are
     * zero.
     */
    std::atomic<uintptr_t> _cown;

    static constexpr uintptr_t COWN_2PL_READY_FLAG = 0x1;
    static constexpr uintptr_t COWN_READER_FLAG = 0x2;
    static constexpr uintptr_t COWN_POINTER_MASK =
      ~(COWN_2PL_READY_FLAG | COWN_READER_FLAG);

    /**
     * Next slot in the MCS Queue
     *
     * Bit 0 - Possible values:
     *          1 - Current slot active
     *          0 - Current slot blocked
     *
     * Bit 1 - Possible values:
     *          0 - Next slot Writer
     *          1 - Next slot Reader
     *
     * Remaining bits  - Next slot pointer
     *
     * Before scheduling: Bit 0 => Whether move or not
     *
     * Assumption - Slots are allocated at 4 byte boundary. Last 2 bits are
     * zero.
     */
    std::atomic<uintptr_t> status;

    static constexpr uintptr_t STATUS_SLOT_ACTIVE_FLAG = 0x1;
    static constexpr uintptr_t STATUS_NEXT_SLOT_READER_FLAG = 0x2;
    static constexpr uintptr_t STATUS_NEXT_SLOT_MASK =
      ~(STATUS_SLOT_ACTIVE_FLAG | STATUS_NEXT_SLOT_READER_FLAG);

    /**
     * Points to the behaviour associated with this slot.
     * This pointer is set only for reader slots.
     * TODO: Change this to get behaviour address from start of allocation if
     * snmalloc is used otherwise use this pointer.
     */
    std::atomic<BehaviourCore*> behaviour;

  public:
    Slot(Cown* __cown)
    {
      // Check that the last two bits are zero
      assert(((uintptr_t)__cown & ~COWN_POINTER_MASK) == 0);
      _cown.store((uintptr_t)__cown, std::memory_order_release);
      status.store(0, std::memory_order_release);
      behaviour.store(nullptr, std::memory_order_release);
    }

    /**
     * Returns true if the slot is acquired in read mode
     */
    bool is_read_only()
    {
      return (_cown.load(std::memory_order_acquire) & COWN_READER_FLAG) ==
        COWN_READER_FLAG;
    }

    /**
     * Mark the slot to be acquired in read mode
     */
    void set_read_only()
    {
      _cown.store(
        _cown.load(std::memory_order_acquire) | COWN_READER_FLAG,
        std::memory_order_release);
    }

    /**
     * Returns true if the next slot wants to acquire in read mode
     */
    bool is_next_slot_read_only()
    {
      return (
        (status.load(std::memory_order_acquire) &
         STATUS_NEXT_SLOT_READER_FLAG) == STATUS_NEXT_SLOT_READER_FLAG);
    }

    /**
     * Returns true if all the slots in a behaviour haven't finished their
     * acquire phase
     */
    bool is_wait_2pl()
    {
      return (_cown.load(std::memory_order_acquire) & COWN_2PL_READY_FLAG) !=
        COWN_2PL_READY_FLAG;
    }

    /**
     * Mark the slot as ready to signify that its acquire phase is complete
     */
    void set_ready()
    {
      _cown.store(
        _cown.load(std::memory_order_acquire) | COWN_2PL_READY_FLAG,
        std::memory_order_release);
    }

    /**
     * Mark the reader slot as active i.e. the behaviour is scheduled.
     * Next reader in the queue can also be scheduled.
     * Returns true if the next is set before active, and contains a reader
     */
    bool set_active_is_next_reader()
    {
      assert(is_read_only());
      yield();
      uintptr_t next =
        status.fetch_add(STATUS_SLOT_ACTIVE_FLAG, std::memory_order_acq_rel);
      return (
        (next & STATUS_NEXT_SLOT_READER_FLAG) == STATUS_NEXT_SLOT_READER_FLAG);
    }

    /**
     * Mark the reader slot as active i.e. the behaviour is scheduled.
     * Next reader in the queue can also be scheduled.
     */
    void set_active()
    {
      assert(is_read_only());
      assert(status == 0);
      yield();
      status.store(STATUS_SLOT_ACTIVE_FLAG, std::memory_order_release);
    }

    /**
     * Get the behaviour associated with the read-only slot
     */
    BehaviourCore* get_behaviour()
    {
      assert(is_read_only());
      assert(behaviour.load(std::memory_order_acquire) != nullptr);
      return behaviour.load(std::memory_order_acquire);
    }

    /**
     * Set the behaviour associated with the read-only slot
     */
    void set_behaviour(BehaviourCore* b)
    {
      assert(is_read_only());
      behaviour.store(b, std::memory_order_release);
    }

    /**
     * Return the next slot
     */
    Slot* next_slot()
    {
      return (
        Slot*)(status.load(std::memory_order_acquire) & STATUS_NEXT_SLOT_MASK);
    }

    /**
     * Returns true if current slot is a writer or a blocked reader,
     * otherwise returns false
     */
    bool set_next_slot_reader(Slot* n)
    {
      // Check that the last two bits are zero
      assert(((uintptr_t)n & ~STATUS_NEXT_SLOT_MASK) == 0);
      assert(next_slot() == nullptr);

      uintptr_t new_status_val =
        ((uintptr_t)n) | (STATUS_NEXT_SLOT_READER_FLAG);

      if (!is_read_only())
      {
        status.store(new_status_val, std::memory_order_seq_cst);
        return true;
      }

      uintptr_t old_status_val =
        status.fetch_add(new_status_val, std::memory_order_seq_cst);
      Logging::cout() << "prev slot is_reader" << is_read_only()
                      << " curr reader " << this
                      << "old_status_val: " << old_status_val
                      << " new_status_val: " << new_status_val << Logging::endl;

      return (
        (old_status_val & STATUS_SLOT_ACTIVE_FLAG) != STATUS_SLOT_ACTIVE_FLAG);
    }

    /**
     * Returns the next behaviour.
     * True only if the next one in the queue is a writer.
     */
    BehaviourCore* next_behaviour()
    {
      assert(!is_next_slot_read_only());
      return (
        BehaviourCore*)(status.load(std::memory_order_acquire) & STATUS_NEXT_SLOT_MASK);
    }

    /**
     * Set the next behaviour
     */
    void set_next_slot_writer(BehaviourCore* b)
    {
      // Check that the last two bits are zero
      assert(((uintptr_t)b & ~STATUS_NEXT_SLOT_MASK) == 0);
      status.store(
        status.load(std::memory_order_acquire) | ((uintptr_t)b),
        std::memory_order_release);
    }

    /**
     * Returns the cown associated with the slot
     */
    Cown* cown()
    {
      return (Cown*)(_cown.load(std::memory_order_acquire) & COWN_POINTER_MASK);
    }

    /**
     * Set the cown pointer to NULL to indicate duplicate cowns within a
     * behaviour.
     */
    void set_cown_null()
    {
      _cown.store(0UL, std::memory_order_release);
    }

    void wakeup_next_writer();

    void release();

    /**
     * Returns true if the slot is acquired with std::move
     */
    bool is_move()
    {
      assert(status.load(std::memory_order_relaxed) <= 1);
      return status.load(std::memory_order_acquire) == 1;
    }

    /**
     * Mark the slot to be acquired with std::move
     */
    void set_move()
    {
      status.store(1, std::memory_order_release);
    }

    /**
     * Mark the slot to be used for scheduling.
     */
    void reset_status()
    {
      status.store(0, std::memory_order_release);
    }

    /**
     * Reset the status of this slot so that it can be rescheduled.
     */
    void reset()
    {
      status.store(0, std::memory_order_release);
      // Make Bit 0 = 0, Mark the cown as blocked on 2PL.
      _cown.store(
        ((_cown.load(std::memory_order_acquire) >> 1) << 1),
        std::memory_order_release);
    }

    inline friend Logging::SysLog& operator<<(Logging::SysLog& os, Slot& s)
    {
      return os
        << " Slot: " << &s << " Cown ptr: "
        << (s._cown.load(std::memory_order_relaxed) & COWN_POINTER_MASK)
        << " 2PL ready bit: "
        << ((s._cown.load(std::memory_order_relaxed) & COWN_2PL_READY_FLAG) ==
            COWN_2PL_READY_FLAG)
        << " Is_reader bit: "
        << ((s._cown.load(std::memory_order_relaxed) & COWN_READER_FLAG) ==
            COWN_READER_FLAG)
        << " Is_Active: "
        << ((s.status.load(std::memory_order_relaxed) &
             STATUS_SLOT_ACTIVE_FLAG) == STATUS_SLOT_ACTIVE_FLAG)
        << " Next pointer: "
        << (s.status.load(std::memory_order_relaxed) & STATUS_NEXT_SLOT_MASK)
        << " Is_next_reader: "
        << ((s.status.load(std::memory_order_relaxed) &
             STATUS_NEXT_SLOT_READER_FLAG) == STATUS_NEXT_SLOT_READER_FLAG)
        << "\n";
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

    inline friend Logging::SysLog&
    operator<<(Logging::SysLog& os, BehaviourCore& b)
    {
      return os << " Behaviour: " << &b << " Cowns: " << b.count
                << " Pending dependencies: "
                << b.exec_count_down.load(std::memory_order_acquire) << " ";
    }

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
     */
    void resolve(size_t n = 1, bool fifo = true)
    {
      Logging::cout() << "Behaviour::resolve " << n << " for behaviour "
                      << *this << Logging::endl;
      // Note that we don't actually perform the last decrement as it is not
      // required.
      if (
        (exec_count_down.load(std::memory_order_acquire) == n) ||
        (exec_count_down.fetch_sub(n) == n))
      {
        Logging::cout() << "Scheduling Behaviour " << *this << Logging::endl;
        Scheduler::schedule(as_work(), fifo);
      }
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

    static void
    acquire_with_transfer(Cown* cown, size_t transfer, size_t required)
    {
      if (transfer == required)
        return;

      if (transfer > required)
      {
        Logging::cout() << "Releasing references as more transferred than "
                           "required: transfer: "
                        << transfer << " required: " << required << " on cown "
                        << cown << Logging::endl;
        // Release transfer - required times, we needed one as we woke up
        // the cown, but the rest were not required.
        for (int j = 0; j < transfer - required; j++)
          Cown::release(ThreadAlloc::get(), cown);
        return;
      }

      Logging::cout() << "Acquiring addition reference count: transfer: "
                      << transfer << " required: " << required << " on cown "
                      << cown << Logging::endl;
      // We didn't have enough RCs passed in, so we need to acquire the rest.
      for (int j = 0; j < required - transfer; j++)
        Cown::acquire(cown);
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
      // We sort first by cown, then by behaviour number and move writers before
      // readers. This means overlaps will be in a sequence in the array in the
      // correct order with respect to the order of the group of behaviours.
      auto compare = [](
                       const std::tuple<size_t, Slot*> i,
                       const std::tuple<size_t, Slot*> j) {
#ifdef USE_SYSTEMATIC_TESTING
        if (std::get<1>(i)->cown()->id() == std::get<1>(j)->cown()->id())
          if (std::get<0>(i) == std::get<0>(j))
            return (!std::get<1>(i)->is_read_only()) &&
              std::get<1>(j)->is_read_only();
          else
            return std::get<0>(i) < std::get<0>(j);
        else
          return std::get<1>(i)->cown()->id() < std::get<1>(j)->cown()->id();
#else
        if (std::get<1>(i)->cown() == std::get<1>(j)->cown())
          if (std::get<0>(i) == std::get<0>(j))
            return (!std::get<1>(i)->is_read_only()) &&
              std::get<1>(j)->is_read_only();
          else
            return std::get<0>(i) < std::get<0>(j);
        else
          return std::get<1>(i)->cown() < std::get<1>(j)->cown();
#endif
      };
      if (count > 1)
        std::sort(indexes.get(), indexes.get() + count, compare);

      // First phase - Acquire phase.
      size_t i = 0;
      while (i < count)
      {
        auto cown = std::get<1>(indexes[i])->cown();
        auto body = bodies[std::get<0>(indexes[i])];
        auto curr_slot = std::get<1>(indexes[i]);
        auto first_body = body;
        size_t first_chain_index = i;

        // The number of RCs provided for the current cown by the when.
        // I.e. how many moves of cown_refs there were.
        size_t transfer_count = curr_slot->is_move();

        Logging::cout() << "Processing " << cown << " " << body << " "
                        << curr_slot << " Index " << i << Logging::endl;

        // Detect duplicates for this cown.
        // This is required in two cases:
        //  * overlaps within a single behaviour.
        while (((++i) < count) && (cown == std::get<1>(indexes[i])->cown()))
        {
          // If the body is the same, then we have an overlap within a single
          // behaviour.
          auto body_next = bodies[std::get<0>(indexes[i])];
          if (body_next == body)
          {
            // Check if the caller passed an RC and add to the total.
            transfer_count += std::get<1>(indexes[i])->is_move();

            Logging::cout() << "Duplicate " << cown << " for " << body
                            << " Index " << i << Logging::endl;
            // We need to reduce the execution count by one, as we can't wait
            // for ourselves.
            ec[std::get<0>(indexes[i])]++;

            // We need to mark the slot as not having a cown associated to it.
            std::get<1>(indexes[i])->set_cown_null();
            continue;
          }

          // For writers, create a chain of behaviours
          if (!std::get<1>(indexes[i])->is_read_only())
          {
            body = body_next;

            // Extend the chain of behaviours linking on this behaviour
            curr_slot->set_next_slot_writer(body);
            curr_slot = std::get<1>(indexes[i]);
            continue;
          }

          // TODO: Chain with reads and writes is not implemented.
          abort();
        }

        // Mark the slot as ready for scheduling
        curr_slot->reset_status();
        yield();
        if (curr_slot->is_read_only())
          curr_slot->set_behaviour(body);
        yield();
        auto prev_slot =
          cown->last_slot.exchange(curr_slot, std::memory_order_acq_rel);
        yield();

        if (prev_slot == nullptr)
        {
          if (curr_slot->is_read_only())
          {
            yield();
            bool first_reader = cown->read_ref_count.add_read();
            Logging::cout() << "Reader at head of queue and got the cown "
                            << *curr_slot << Logging::endl;
            yield();
            curr_slot->set_active();
            ec[std::get<0>(indexes[first_chain_index])]++;
            yield();
            acquire_with_transfer(cown, transfer_count, 1 + first_reader);
            continue;
          }

          yield();

          acquire_with_transfer(cown, transfer_count, 1);

          yield();

          if (cown->read_ref_count.try_write())
          {
            Logging::cout() << " Writer at head of queue and got the cown "
                            << *curr_slot << Logging::endl;
            ec[std::get<0>(indexes[first_chain_index])]++;
            yield();
            continue;
          }

          Logging::cout() << " Writer waiting for previous readers cown "
                          << *curr_slot << Logging::endl;
          yield();
          cown->next_writer = body;
          continue;
        }

        // Release any transferred count
        acquire_with_transfer(cown, transfer_count, 0);

        yield();
        Logging::cout() << " Someone in queue cown " << *curr_slot
                        << " previous " << *prev_slot << Logging::endl;

        while (prev_slot->is_wait_2pl())
        {
          Systematic::yield_until(
            [prev_slot]() { return !prev_slot->is_wait_2pl(); });
          Aal::pause();
        }

        if (curr_slot->is_read_only())
        {
          if (prev_slot->set_next_slot_reader(curr_slot))
          {
            Logging::cout()
              << " Previous slot is a writer or blocked reader cown "
              << *curr_slot << Logging::endl;
            yield();
            continue;
          }

          yield();
          bool first_reader = cown->read_ref_count.add_read();
          Logging::cout() << " Reader got the cown " << *curr_slot
                          << Logging::endl;
          yield();
          curr_slot->set_active();
          ec[std::get<0>(indexes[first_chain_index])]++;
          if (first_reader)
          {
            Logging::cout()
              << "Acquiring reference count for first reader on cown "
              << *curr_slot << Logging::endl;
            Cown::acquire(cown);
          }
          continue;
        }

        Logging::cout()
          << " Writer waiting for cown. Set next of previous slot cown "
          << *curr_slot << " previous " << *prev_slot << Logging::endl;
        prev_slot->set_next_slot_writer(first_body);
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
        if (slot->is_wait_2pl())
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
      Logging::cout() << "Finished Behaviour " << *this << Logging::endl;
      auto slots = get_slots();
      // Behaviour is done, we can resolve successors.
      for (size_t i = 0; i < count; i++)
      {
        slots[i].release();
      }
      Logging::cout() << "Finished Resolving successors " << *this
                      << Logging::endl;
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

  inline void Slot::wakeup_next_writer()
  {
    auto w = cown()->next_writer.load();

    if (w == nullptr)
    {
      while (cown()->next_writer.load() == nullptr)
      {
        Systematic::yield_until(
          [this]() { return cown()->next_writer.load() != nullptr; });
        Aal::pause();
      }

      w = cown()->next_writer.load();
    }

    Logging::cout() << *this << " Last Reader waking up next writer " << *w
                    << Logging::endl;

    yield();
    cown()->next_writer = nullptr;
    w->resolve();
  }

  inline void Slot::release()
  {
    Logging::cout() << "Release slot " << *this << Logging::endl;

    assert(!is_wait_2pl());

    // This slot represents a duplicate cown, so we can ignore releasing it.
    if (cown() == nullptr)
    {
      Logging::cout() << "Duplicate cown slot " << *this << Logging::endl;
      return;
    }

    if (next_slot() == nullptr)
    {
      auto slot_addr = this;
      if (cown()->last_slot.compare_exchange_strong(
            slot_addr, nullptr, std::memory_order_acq_rel))
      {
        yield();

        if (is_read_only())
        {
          auto status = cown()->read_ref_count.release_read();
          if (status != ReadRefCount::NOT_LAST)
          {
            if (status == ReadRefCount::LAST_READER_WAITING_WRITER)
            {
              Logging::cout()
                << *this << "Last Reader releasing the cown with writer waiting"
                << Logging::endl;

              // Last reader
              yield();
              wakeup_next_writer();
            }

            // Release cown as this will be set by the new thread joining the
            // queue.
            Logging::cout() << *this << " Last reader No more work for cown "
                            << Logging::endl;
            shared::release(ThreadAlloc::get(), cown());
          }
        }

        yield();
        // Release cown as this will be set by the new thread joining the
        // queue.
        Logging::cout() << *this << " CAS Success No more work for cown "
                        << Logging::endl;
        shared::release(ThreadAlloc::get(), cown());
        return;
      }

      // If we failed, then the another thread is extending the chain
      while (next_slot() == nullptr)
      {
        Systematic::yield_until([this]() { return (next_slot() != nullptr); });
        Aal::pause();
      }
    }

    if (is_read_only())
    {
      yield();

      if (!is_next_slot_read_only())
      {
        yield();

        Logging::cout() << *this << "Reader setting next writer variable "
                        << next_behaviour() << Logging::endl;
        /*
        For a chain of readers, only the last reader in the chain
        will find the next slot as the writer and will set the next_writer
        variable. Hence, this store is not atomic.
        */
        if (cown()->read_ref_count.try_write())
        {
          next_behaviour()->resolve();
        }
        else
        {
          cown()->next_writer = next_behaviour();
        }

        yield();
      }

      Logging::cout() << *this << " Reader releasing the cown "
                      << Logging::endl;

      auto status = cown()->read_ref_count.release_read();
      if (status != ReadRefCount::NOT_LAST)
      {
        // Last reader
        if (status == ReadRefCount::LAST_READER_WAITING_WRITER)
        {
          yield();
          wakeup_next_writer();
        }
        // Release cown as this will be set by the new thread joining the
        // queue.
        Logging::cout() << *this << " Last reader No more work for cown "
                        << Logging::endl;
        shared::release(ThreadAlloc::get(), cown());
      }
      return;
    }

    if (!is_next_slot_read_only())
    {
      Logging::cout() << *this
                      << " Writer waking up next writer cown next slot "
                      << *next_behaviour() << Logging::endl;
      next_behaviour()->resolve();
      return;
    }

    std::vector<Slot*> reader_queue;
    bool first_reader = cown()->read_ref_count.add_read();

    yield();

    Logging::cout() << *this
                    << " Writer waking up next reader and acquiring "
                       "reference count for first reader. next slot "
                    << *next_slot() << Logging::endl;
    assert(first_reader);
    Cown::acquire(cown());
    yield();

    Slot* curr_slot = next_slot();
    while (true)
    {
      reader_queue.push_back(curr_slot);
      if (!curr_slot->set_active_is_next_reader())
        break;
      yield();
      curr_slot = curr_slot->next_slot();
    }

    // Add read count for readers. First reader is already added in rcount
    cown()->read_ref_count.add_read(reader_queue.size() - 1);

    yield();

    for (auto reader : reader_queue)
    {
      reader->get_behaviour()->resolve(1, false);
      yield();
    }
  }
} // namespace verona::rt
