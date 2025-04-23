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

  class BehaviourCore;

  inline Logging::SysLog& operator<<(Logging::SysLog&, BehaviourCore&);

  class Slot
  {
    friend BehaviourCore;

  private:
    /**
     * Cown required by this behaviour
     *
     * Bit 0 - Set means this carries an RC. I.e. came from a move.
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
    uintptr_t _cown;

    static constexpr uintptr_t COWN_MOVE_FLAG = 0x1;
    static constexpr uintptr_t COWN_READER_FLAG = 0x2;
    static constexpr uintptr_t COWN_POINTER_MASK =
      ~(COWN_READER_FLAG | COWN_MOVE_FLAG);

    /**
     * Next slot in the MCS Queue
     *
     * This encodes the following possible values:
     *   - Wait - 2PL means that successor should spin on this
     *   - Ready - 2PL completed and a successor can be added
     *   - ReadAvailable - 2PL completed and the cown is read-only mode
     *        with no writing waiters.
     *   - ChainClosed - The successor has taken over the chain, and when the
     *        behaviour that contains this slot completes it can drop the chain.
     *   - Read(Slot*) - A pointer to the next slot in the read chain
     *   - Write(BehaviourCore*) - A pointer to the next behaviour that is
     *        waiting in Write mode.
     *
     * The ReadAvailable status means that subsequent readers can start
     * immediately without waiting for the current reader to finish.  A
     * subsequent Writer must add itself to the cowns next_writer property to be
     * scheduled, when the readers complete.
     *
     * To distinguish Read and Write we use the bottom bit.
     *
     * The state transitions are as follows.
     * For the scheduler of the behaviour for this slot:
     *  - Wait          -> Ready          (Uncontended)
     *  - Wait          -> ReadAvailable  (Uncontended)
     *  - Ready         -> ReadAvailable
     * From the successor behaviour:
     *  - Ready         -> Read(Slot*)
     *  - Ready         -> Write(BehaviourCore*)
     *  - ReadAvailable -> ChainClosed    (Uncontended)
     * The only contended state is Ready, where there can be a race between
     * the successor trying to add a Read/Write and the current behaviour
     * trying to set the ReadAvailable status.
     */
    std::atomic<uintptr_t> status;

    // Current slot initial status
    static constexpr uintptr_t STATUS_WAIT = 0x0;
    // Completed 2PL
    static constexpr uintptr_t STATUS_READY = 0x1;
    // Completed 2PL, and end of a executing read chain
    static constexpr uintptr_t STATUS_READAVAILABLE = 0x2;
    // This is set by the successor to acknowledge that it has taken over the
    // chain, and the current slot does not need to notify the next slot.
    // Importantly this is larger than the other statuses, so that the check
    // for a successor response can be it is set to 0x3 or larger.
    static constexpr uintptr_t STATUS_CHAIN_CLOSED = 0x3;
    // Use the bottom bit of a pointer to indicate that the pointer is to a
    // reader slot, rather than a writer behaviour.
    static constexpr uintptr_t STATUS_READ_FLAG = 0x1;
    static constexpr uintptr_t STATUS_NEXT_SLOT_MASK = ~(0x3);

    /**
     * Points to the behaviour associated with this slot.
     * This pointer is set only for reader slots.
     * TODO: Change this to get behaviour address from start of allocation if
     * snmalloc is used otherwise use this pointer.
     */
    BehaviourCore* behaviour;

    /**
     * Returns 1 if the slot is acquired with std::move. It clears the flag.
     * Returns 0 otherwise.
     */
    size_t take_move()
    {
      assert(status.load(std::memory_order_relaxed) <= 1);
      if ((_cown & COWN_MOVE_FLAG) == 0)
        return 0;
      _cown &= ~COWN_MOVE_FLAG;
      return 1;
    }

    /**
     * Returns true if the next slot wants to acquire in read mode
     */
    bool is_next_slot_read_only()
    {
      assert(status > STATUS_CHAIN_CLOSED);
      return ((status.load(std::memory_order_acquire) & STATUS_READ_FLAG) != 0);
    }

    /**
     * Returns true if all the slots in a behaviour haven't finished their
     * acquire phase
     */
    bool is_wait_2pl()
    {
      return (status.load(std::memory_order_acquire)) == STATUS_WAIT;
    }

    /**
     * Mark the slot as ready to signify that its acquire phase is complete
     */
    void set_ready()
    {
      Logging::cout() << "set_ready " << *this << Logging::endl;
      status.store(STATUS_READY, std::memory_order_release);
    }

    /**
     * Mark the reader slot as read available i.e. the behaviour is scheduled.
     * Next reader in the queue can also be scheduled.
     * Returns false, if there is a successor already, and the setting failed.
     */
    [[nodiscard]] bool set_read_available_contended()
    {
      yield();
      Logging::cout() << "set_read_available_contended " << *this
                      << Logging::endl;
      assert(is_read_only());
      assert(status != STATUS_WAIT);
      assert(status != STATUS_READAVAILABLE);
      uintptr_t old_status = STATUS_READY;
      return (status.load(std::memory_order_acquire) == old_status) &&
        status.compare_exchange_strong(old_status, STATUS_READAVAILABLE);
    }

    /**
     * Mark the reader slot as read available i.e. the behaviour is scheduled.
     * Next reader in the queue can also be scheduled.
     */
    void set_read_available_uncontended()
    {
      yield();
      Logging::cout() << "set_read_available " << this << " status "
                      << (void*)status.load() << Logging::endl;
      assert(is_read_only());
      assert(status == STATUS_WAIT);
      status.store(STATUS_READAVAILABLE, std::memory_order_release);
    }

    /**
     * Get the behaviour associated with the read-only slot
     */
    BehaviourCore* get_behaviour()
    {
      assert(is_read_only());
      assert(behaviour != nullptr);
      return behaviour;
    }

    /**
     * Set the behaviour associated with the read-only slot
     */
    void set_behaviour(BehaviourCore* b)
    {
      assert(is_read_only());
      behaviour = b;
    }

    /**
     * Return the next slot
     */
    Slot* next_slot()
    {
      assert(is_next_slot_read_only());
      return (
        Slot*)(status.load(std::memory_order_acquire) & STATUS_NEXT_SLOT_MASK);
    }

    /**
     * Returns true if this slot has not been updated by the sucessor.
     */
    bool no_successor_response()
    {
      return (status.load(std::memory_order_acquire)) < STATUS_CHAIN_CLOSED;
    }

    /**
     * Returns true if successful.
     * Return false if failed due to READ_AVAILBLE being set.
     */
    void set_next_slot_reader_uncontended(Slot* n)
    {
      Logging::cout() << "set_next_slot_reader_uncontended " << this
                      << " status " << (void*)status.load() << Logging::endl;
      // Should only be called when neither the read-available nor reader bit
      // have been set.
      assert(status == STATUS_WAIT);

      status = ((uintptr_t)n) | (STATUS_READ_FLAG);
    }

    /**
     * Returns true if successful.
     * Return false if failed due to READ_AVAILABLE being set.
     */
    [[nodiscard]] bool set_next_slot_reader_contended(Slot* n)
    {
      Logging::cout() << "set_next_slot_reader_contended " << this << " status "
                      << (void*)status.load() << Logging::endl;
      // Should only be called when neither the read-available nor reader bit
      // have been set.
      assert(((uintptr_t)n & ~STATUS_NEXT_SLOT_MASK) == 0);
      assert(no_successor_response());
      assert(!is_wait_2pl());

      uintptr_t new_status = ((uintptr_t)n) | (STATUS_READ_FLAG);
      uintptr_t old_status = STATUS_READY;

      bool success = (status.load() == old_status) &&
        status.compare_exchange_strong(
          old_status, new_status, std::memory_order_acq_rel);

      Logging::cout() << *this << Logging::endl;

      if (success)
      {
        Logging::cout() << "set_next_slot_reader success" << Logging::endl;
      }
      else
      {
        Logging::cout() << "set_next_slot_reader failed" << Logging::endl;
        status = STATUS_CHAIN_CLOSED;
      }

      return success;
    }

    /**
     * Should only be called when the next slot is a writer.
     *
     * This returns the next behaviour.
     */
    BehaviourCore* next_behaviour()
    {
      assert(!is_next_slot_read_only());
      return (
        BehaviourCore*)(status.load(std::memory_order_acquire) & STATUS_NEXT_SLOT_MASK);
    }

    /**
     * Set the next behaviour to be a writer during prepare phase.
     */
    void set_next_slot_writer_uncontended(BehaviourCore* b)
    {
      Logging::cout() << "set_next_slot_writer_uncontended " << this
                      << " status " << (void*)status.load() << Logging::endl;
      // Requires that neither the READONLY or read-available bits are set.
      assert(((uintptr_t)b & ~STATUS_NEXT_SLOT_MASK) == 0);
      assert(status == STATUS_WAIT);

      status = (uintptr_t)b;
    }

    /**
     * Set the next behaviour to be a writer.
     *
     * Returns true if successful.
     * Returns false if it failed as the ReadAvailable was set.
     */
    [[nodiscard]] bool set_next_slot_writer_contended(BehaviourCore* b)
    {
      Logging::cout() << "set_next_slot_writer_contended " << this << " status "
                      << (void*)status.load() << Logging::endl;
      // Requires that neither the READONLY or read-available bits are set.
      assert(((uintptr_t)b & ~STATUS_NEXT_SLOT_MASK) == 0);

      // Read available is never set on a writer, so there is no contention here.
      if (!is_read_only())
      {
        status = (uintptr_t)b;
        return true;
      }

      uintptr_t old_value = STATUS_READY;
      auto success = (status.load(std::memory_order_acquire) == old_value) &&
        status.compare_exchange_strong(
          old_value, (uintptr_t)b, std::memory_order_acq_rel);

      if (success)
      {
        Logging::cout() << "set_next_slot_writer success" << Logging::endl;
      }
      else
      {
        Logging::cout() << "set_next_slot_writer failed" << Logging::endl;
        status = STATUS_CHAIN_CLOSED;
      }
      return success;
    }

    /**
     * Set the cown pointer to NULL to indicate duplicate cowns within a
     * behaviour.
     */
    void set_cown_null()
    {
      _cown = 0;
    }

    void wakeup_next_writer();

    void drop_read();

    inline friend Logging::SysLog& operator<<(Logging::SysLog& os, Slot& s)
    {
      os << "Slot: " << &s << Logging::endl;

      os << "-  Cown ptr: " << (Cown*)(s._cown & COWN_POINTER_MASK)
         << Logging::endl;

      os << "-  Is Move bit: " << ((s._cown & COWN_MOVE_FLAG) != 0)
         << Logging::endl;

      os << "-  Is_reader bit: " << ((s._cown & COWN_READER_FLAG) != 0)
         << Logging::endl;

      os << "-  status: " << (void*)(s.status.load()) << Logging::endl;

      return os;
    }

  public:
    Slot(Cown* __cown, bool ready = false)
    {
      // Check that the last two bits are zero
      assert(((uintptr_t)__cown & ~COWN_POINTER_MASK) == 0);
      _cown = (uintptr_t)__cown;
      status.store(
        ready ? STATUS_READY : STATUS_WAIT, std::memory_order_release);
      behaviour = nullptr;

      Logging::cout() << "Slot created " << *this << Logging::endl;
    }

    /**
     * Returns the cown associated with the slot
     */
    Cown* cown()
    {
      return (Cown*)(_cown & COWN_POINTER_MASK);
    }

    /**
     * Returns true if the slot is acquired in read mode
     */
    bool is_read_only()
    {
      return (_cown & COWN_READER_FLAG) == COWN_READER_FLAG;
    }

    /**
     * Mark the slot to be acquired in read mode
     */
    void set_read_only()
    {
      Logging::cout() << "set_read_only " << *this << Logging::endl;

      _cown |= COWN_READER_FLAG;

      Logging::cout() << "set_read_only finished: " << *this << Logging::endl;
    }

    /**
     * Mark the slot to be acquired with std::move
     */
    void set_move()
    {
      _cown |= COWN_MOVE_FLAG;
    }

    /**
     * Mark the slot to be used for scheduling.
     */
    void reset_status()
    {
      status.store(STATUS_WAIT, std::memory_order_release);
    }

    /**
     * Reset the status of this slot so that it can be rescheduled.
     */
    void reset()
    {
      status.store(STATUS_WAIT, std::memory_order_release);
    }

    /**
     * TODO This should not be part of the public API, but the C++ promise
     * experiment needs it.  We should add sufficient API for promises into
     * the core, and then remove this from the public API.
     */
    void release();
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
  class BehaviourCore
  {
    friend Slot;
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

    /**
     * This function is used to acquire enough references to the cown.
     *
     *  - cown is the cown to acquire references to.
     *  - transfer is how many references we already have, i.e. how many have
     *    been transferred from the context.
     *  - required is how many references we now need.
     *
     * This can result in either increasing the reference count, or releasing
     * references.
     */
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
          Cown::release(cown);
        return;
      }

      Logging::cout() << "Acquiring addition reference count: transfer: "
                      << transfer << " required: " << required << " on cown "
                      << cown << Logging::endl;
      // We didn't have enough RCs passed in, so we need to acquire the rest.
      for (int j = 0; j < required - transfer; j++)
        Cown::acquire(cown);
    }

    static std::tuple<size_t, bool> handle_read_only_enqueue(
      Slot* prev_slot,
      Slot* chain_first_slot,
      Slot* chain_last_slot,
      size_t first_consecutive_readers_count,
      Cown* cown)
    {
      size_t ref_count = 0;
      bool first_reader;

      if (
        prev_slot &&
        (prev_slot->set_next_slot_reader_contended(chain_first_slot)))
      {
        Logging::cout() << " Previous slot is a writer or blocked reader cown "
                        << *chain_first_slot << Logging::endl;
        yield();
        return {0, false};
      }

      yield();
      first_reader =
        cown->read_ref_count.add_read(first_consecutive_readers_count);
      Logging::cout() << " Reader got the cown " << *chain_first_slot
                      << Logging::endl;
      yield();

      if (first_reader)
      {
        ref_count = 1;
      }

      return {ref_count, true};
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

  public:
    // TODO: When C++ 20 move to span.
    Slot* get_slots()
    {
      return pointer_offset<Slot>(this, sizeof(BehaviourCore));
    }

    size_t get_count()
    {
      return count;
    }

    template<typename T = void>
    T* get_body()
    {
      Slot* slots = pointer_offset<Slot>(this, sizeof(BehaviourCore));
      return pointer_offset<T>(slots, sizeof(Slot) * count);
    }

    /**
     * @brief Gets the pointer to the closure body from the work object.
     * This takes a template parameter as this almost always needs casting
     * to the correct type.
     */
    template<typename T = void>
    static T* body_from_work(Work* w)
    {
      return reinterpret_cast<T*>(from_work(w)->get_body());
    }

    /**
     * @brief Called on completion of a behaviour.  This will release the slots
     * so that subsequent behaviours can be scheduled.
     * @param work - The work object that was used to schedule the behaviour.
     * @param reuse - If true, then the behaviour will be reset and reused.
     * Otherwise, it will be deallocated.
     */
    static void finished(Work* work, bool reuse = false)
    {
      auto behaviour = BehaviourCore::from_work(work);
      Logging::cout() << "Finished Behaviour " << *behaviour << Logging::endl;
      behaviour->release_all();
      if (!reuse)
        heap::dealloc(work);
      else
        behaviour->reset();
    }

    /**
     * @brief Deallocate the behaviour.
     *
     * This will deallocate the work object, and the body of the behaviour.
     * This only needs to be called for behaviours that called finished(...,
     * true) as the finished function will not have deallocated the work object
     * and behaviour.
     */
    void dealloc()
    {
      Logging::cout() << "Deallocating Behaviour " << *this << Logging::endl;
      heap::dealloc(as_work());
    }

    /**
     * @brief Constructs a behaviour.  Leaves space for the closure.
     *
     * @param count - Number of slots to allocate, i.e. how many cowns to wait
     * for.
     * @param f - The function to execute once all the behaviours dependencies
     * are ready.  This should have a specific form as it will receive a pointer
     * to work object rather than body itself.
     * @param payload - The size of the payload to allocate.
     * @return BehaviourCore* - the pointer to the behaviour object.
     *
     * @note
     * The work function of f should be of the form:Aal
     *
     *   void invoke(Work*)
     *   {
     *     Body* body = BehaviourCore::body_from_work<Body>(work);
     *
     *     // Do the actual behaviours work
     *     ...
     *
     *     BehaviourCore::finished(work);
     *   }
     *
     *  Using this form allows the implementation to use a single indirect call
     *  to this function, rather than having to do a second indirect call inside
     *  the body of the behaviour for what to do.  (Note the underlying
     * scheduler runs things other than behaviours, so it will alway need at
     * least one indirect call).
     *
     * @note The behaviour does not fill in the slots for the cowns, and those
     * should be filled in by the caller.
     *
     *    BehaviourCore b = make(2, invoke, sizeof(Body));
     *    b.get_slots()[0] = Slot(cown1);
     *    b.get_slots()[1] = Slot(cown2);
     *
     *    BehaviourCore::schedule_many(&b, 1);
     *
     * This fills in the two slots, and then schedules the behaviour.
     */
    static BehaviourCore* make(size_t count, void (*f)(Work*), size_t payload)
    {
      // Manual memory layout of the behaviour structure.
      //   | Work | Behaviour | Slot ... Slot | Body |
      size_t size =
        sizeof(Work) + sizeof(BehaviourCore) + (sizeof(Slot) * count) + payload;
      void* base = heap::alloc(size);

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
     *  @brief Atomically schedule a collection of behaviours for
     * execution.
     * @param bodies An array of behaviours to schedule
     *
     * @param body_count The number of behaviours to schedule.
     *
     * @note This adds the behaviours to the dependency graph, and handles all
     * the process of waking up the work and adding to the underlying scheduler.
     */
    static void schedule_many(BehaviourCore** bodies, size_t body_count)
    {
      /* IMPLEMENTATION NOTE
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
       * previous behaviour on that cown specifies it has completed its
       * scheduling by marking itself ready, `set_ready`.
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
       * Invariant: If the cown is part of a chain, then the scheduler holds an
       * RC on the cown. This means the first behaviour to access a cown will
       * need to perform an acquire. When the execution of a chain completes,
       * then the scheduler will release the RC.
       *
       * *** Extension to Many ***
       *
       * This code additional can schedule a group of behaviours atomically.
       *
       *   when (a) { B1 } + when (b) { B2 } + when (a, b) { B3 }
       *
       * This will cause the three behaviours to be scheduled in a single atomic
       * step using the two phase commit.  This means that no other behaviours
       * can access a between B1 and B3, and no other behaviours can access b
       * between B2 and B3.
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
       * We then link the (0,a) |-> (2,a), and enqueue the segment atomically
       * onto cown a, and then link (1,b) |-> (2,b) and enqueue the segment
       * atomically onto cown b.
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
       * as not needing a successor.  This is done by setting the cown pointer
       * to nullptr.
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
      Logging::cout() << "BehaviourCore::schedule_many" << body_count
                      << Logging::endl;

      // non-unique cowns count
      size_t cown_count = 0;
      for (size_t i = 0; i < body_count; i++)
        cown_count += bodies[i]->count;

      // Execution count - we will remove at least
      // one from the execution count on finishing phase 2 of the
      // 2PL. This ensures that the behaviour cannot be
      // deallocated until we finish phase 2.
      StackArray<size_t> ec(body_count);
      for (size_t i = 0; i < body_count; i++)
        ec[i] = 1;

      // This array includes an entry for each of the requested cowns.
      // The entry has an index into the bodies array and a Slot * for that
      // cown inside the behaviour body
      // Need to sort the cown requests across the co-scheduled collection of
      // cowns We first construct an array that represents pairs of behaviour
      // number and slot pointer. Note: Really want a dynamically sized stack
      // allocation here.
      StackArray<std::tuple<size_t, Slot*>> cown_to_behaviour_slot_map(
        cown_count);
      size_t idx = 0;
      for (size_t i = 0; i < body_count; i++)
      {
        auto slots = bodies[i]->get_slots();
        for (size_t j = 0; j < bodies[i]->count; j++)
        {
          cown_to_behaviour_slot_map[idx] = {i, &slots[j]};
          idx++;
        }
      }

      // First phase - Prepare phase
      // For each unique cown, build a chain of behaviours that need to be
      // scheduled.
      // First order the cowns to find the unique ones and the prepare the
      // chains

      // Sort the indexing array so we make the requests in the correct order
      // across the whole set of behaviours.  A consistent order is required to
      // avoid deadlock.
      // We sort first by cown, then by behaviour number and move writers before
      // readers. This means overlaps will be in a sequence in the array in the
      // correct order with respect to the order of the group of behaviours.
      //
      // The challenging case is given by the following example:
      //
      //   when (read(a)) { ... } +  when (read(a), a) { ... } + when(a) { ... }
      //
      // Here we have three slots (0,0), (1,0), (1,1), (2,0) and we need to
      // ensure that the resulting order is
      //    (0,0), (1,1), (1,0), (2,0)
      // and then we can drop (1,0).
      // This is because the second behaviour needs write access, so that has to
      // be prioritised over the read, but between behaviours, we should keep
      // the order the same. This means we can always ignore anything but the
      // first slot for each behaviour when building the dependency chain.
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
      if (cown_count > 1)
        std::sort(
          cown_to_behaviour_slot_map.get(),
          cown_to_behaviour_slot_map.get() + cown_count,
          compare);

      // Helper struct to be used after building the chains in the next phases
      struct ChainInfo
      {
        Cown* cown;
        size_t first_body_index;
        Slot* first_slot;
        Slot* last_slot;
        size_t transfer_count;
        bool had_no_predecessor;
        size_t ref_count;
        bool read_only_can_run;
        BehaviourCore* first_writer;
        size_t first_consecutive_readers_count;
      };
      size_t i = 0;
      size_t chain_count = 0;
      StackArray<ChainInfo> chain_info(cown_count);

      while (i < cown_count)
      {
        auto cown = std::get<1>(cown_to_behaviour_slot_map[i])->cown();
        auto body = bodies[std::get<0>(cown_to_behaviour_slot_map[i])];

        auto first_slot = std::get<1>(cown_to_behaviour_slot_map[i]);
        size_t first_body_index = std::get<0>(cown_to_behaviour_slot_map[i]);

        // The number of RCs provided for the current cown by the when.
        // I.e. how many moves of cown_refs there were.
        size_t transfer_count = first_slot->take_move();

        BehaviourCore* first_writer =
          first_slot->is_read_only() ? nullptr : body;

        Logging::cout() << "Processing " << (Cown*)cown << " " << body << " "
                        << first_slot << " Index " << i << Logging::endl;

        auto curr_slot = first_slot;

        size_t first_consecutive_readers_count =
          first_writer == nullptr ? 1 : 0;

        // Detect duplicates for this cown.
        // This is required in two cases:
        //  * overlaps within a single behaviour.
        while (((++i) < cown_count) &&
               (cown == std::get<1>(cown_to_behaviour_slot_map[i])->cown()))
        {
          // If the body is the same, then we have an overlap within a single
          // behaviour.
          auto body_next = bodies[std::get<0>(cown_to_behaviour_slot_map[i])];

          // Check if the caller passed an RC and add to the total.
          transfer_count +=
            std::get<1>(cown_to_behaviour_slot_map[i])->take_move();
          auto slot_next = std::get<1>(cown_to_behaviour_slot_map[i]);
          if (body_next == body)
          {
            // Check if the caller passed an RC and add to the total.
            transfer_count +=
              std::get<1>(cown_to_behaviour_slot_map[i])->take_move();

            Logging::cout() << "Duplicate " << cown << " for " << body
                            << " Index " << i << Logging::endl;
            // We need to reduce the execution count by one, as we can't wait
            // for ourselves.
            ec[std::get<0>(cown_to_behaviour_slot_map[i])]++;

            // We need to mark the slot as not having a cown associated to it.
            std::get<1>(cown_to_behaviour_slot_map[i])->set_cown_null();
            continue;
          }

          if (slot_next->is_read_only())
          {
            // Extend the chain of behaviours linking on this behaviour
            curr_slot->set_next_slot_reader_uncontended(slot_next);

            if (first_writer == nullptr)
              first_consecutive_readers_count++;
          }
          else
          {
            if (first_writer == nullptr)
              first_writer = body_next;

            // Extend the chain of behaviours linking on this behaviour
            curr_slot->set_next_slot_writer_uncontended(body_next);
          }

          if (curr_slot->is_read_only())
            curr_slot->set_behaviour(body);

          body = body_next;
          curr_slot = slot_next;
        }

        // Set the behaviour for the last slot in the chain
        if (curr_slot->is_read_only())
          curr_slot->set_behaviour(body);

        // For each chain you need a bunch of data used for scheduling
        chain_info[chain_count++] = {
          cown,
          first_body_index,
          first_slot,
          curr_slot,
          transfer_count,
          false,
          0,
          false,
          first_writer,
          first_consecutive_readers_count};

        // Mark the slot as ready for scheduling
        curr_slot->reset_status();
        yield();
      }

      // Second phase - Acquire phase
      for (size_t i = 0; i < chain_count; i++)
      {
        auto* cown = chain_info[i].cown;
        auto first_body_index = chain_info[i].first_body_index;
        auto* first_body = bodies[first_body_index];
        auto* chain_last_slot = chain_info[i].last_slot;
        auto* chain_first_slot = chain_info[i].first_slot;

        auto prev_slot =
          cown->last_slot.exchange(chain_last_slot, std::memory_order_acq_rel);

        yield();

        if (prev_slot == nullptr)
        {
          chain_info[i].had_no_predecessor = true;
          if (chain_first_slot->is_read_only())
          {
            auto enqueue_res = handle_read_only_enqueue(
              prev_slot,
              chain_first_slot,
              chain_last_slot,
              chain_info[i].first_consecutive_readers_count,
              cown);
            chain_info[i].ref_count = std::get<0>(enqueue_res);
            chain_info[i].read_only_can_run = std::get<1>(enqueue_res);
          }
          continue;
        }

        while (prev_slot->is_wait_2pl())
        {
          Systematic::yield_until(
            [prev_slot]() { return !prev_slot->is_wait_2pl(); });
          Aal::pause();
        }

        if (chain_first_slot->is_read_only())
        {
          auto enqueue_res = handle_read_only_enqueue(
            prev_slot,
            chain_first_slot,
            chain_last_slot,
            chain_info[i].first_consecutive_readers_count,
            cown);
          chain_info[i].ref_count = std::get<0>(enqueue_res);
          chain_info[i].read_only_can_run = std::get<1>(enqueue_res);
          continue;
        }

        Logging::cout()
          << " Writer waiting for cown. Set next of previous slot cown "
          << *chain_last_slot << " previous " << *prev_slot << Logging::endl;
        if (!prev_slot->set_next_slot_writer_contended(first_body))
        {
          yield();
          // The previous slot had read available, we need to add ourselves as
          // the next_writer to the cown, etc.
          chain_info[i].read_only_can_run = true;
        }
        yield();
      }

      // Third phase - Release phase.
      for (size_t i = 0; i < body_count; i++)
      {
        Logging::cout() << "Release phase for behaviour " << bodies[i]
                        << Logging::endl;
      }

      for (size_t i = 0; i < chain_count; i++)
      {
        yield();
        auto slot = chain_info[i].last_slot;
        if (chain_info[i].had_no_predecessor || chain_info[i].read_only_can_run)
        {
          if (chain_info[i].first_writer == nullptr)
          {
            Logging::cout() << "Setting slot " << slot << " to read available."
                            << Logging::endl;
            slot->set_read_available_uncontended();
            continue;
          }
        }
        Logging::cout() << "Setting slot " << slot << " to ready"
                        << Logging::endl;
        slot->set_ready();
      }

      // Fourth phase - Process & Resolve

      for (size_t i = 0; i < chain_count; i++)
      {
        auto* cown = chain_info[i].cown;
        auto first_body_index = chain_info[i].first_body_index;
        auto* first_body = bodies[first_body_index];
        auto* chain_first_slot = chain_info[i].first_slot;
        auto chain_had_no_predecessor = chain_info[i].had_no_predecessor;
        auto transfer_count = chain_info[i].transfer_count;
        auto ref_count = chain_info[i].ref_count;
        auto read_only_can_run = chain_info[i].read_only_can_run;
        auto first_consecutive_readers_count =
          chain_info[i].first_consecutive_readers_count;
        auto first_writer = chain_info[i].first_writer;

        // Process reference count
        if (chain_had_no_predecessor)
        {
          ref_count++;
        }
        acquire_with_transfer(cown, transfer_count, ref_count);

        // Process writes without predecessor
        if ((chain_had_no_predecessor || read_only_can_run))
        {
          if (!chain_first_slot->is_read_only())
          {
            if (cown->read_ref_count.try_write())
            {
              Logging::cout() << " Writer at head of queue and got the cown "
                              << *chain_first_slot << Logging::endl;
              // Process execution count
              ec[first_body_index] += 1;
              yield();
            }
            else
            {
              Logging::cout() << " Writer waiting for previous readers cown "
                              << *chain_first_slot << Logging::endl;
              yield();
              cown->next_writer = first_body;
            }

            continue;
          }

          if (first_writer != nullptr)
          {
            auto result = cown->read_ref_count.try_write();
            // There should definitely be at least one reader in the chain.
            assert(!result);
            snmalloc::UNUSED(result);
            cown->next_writer = first_writer;
          }
        }

        // This part is only for chains that start with a read-only behaviour
        if (read_only_can_run)
        {
          for (int i = 0; i < first_consecutive_readers_count; i++)
            ec[first_body_index + i] += 1;
        }
      }

      for (size_t i = 0; i < body_count; i++)
      {
        yield();
        bodies[i]->resolve(ec[i]);
      }
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

  inline void Slot::drop_read()
  {
    assert(is_read_only());

    auto status = cown()->read_ref_count.release_read();
    if (status != ReadRefCount::NOT_LAST)
    {
      if (status == ReadRefCount::LAST_READER_WAITING_WRITER)
      {
        Logging::cout() << *this
                        << "Last Reader releasing the cown with writer waiting"
                        << Logging::endl;

        // Last reader
        yield();
        wakeup_next_writer();
      }

      // Release cown as this will be set by the new thread joining the
      // queue.
      Logging::cout() << *this
                      << " Last Reader releasing the cown no writer waiting"
                      << Logging::endl;
      shared::release(cown());
    }
  }

  inline void Slot::release()
  {
    Logging::cout() << "Release slot " << *this << Logging::endl;

    // This slot represents a duplicate cown, so we can ignore releasing it.
    if (cown() == nullptr)
    {
      Logging::cout() << "Duplicate cown slot " << Logging::endl;
      return;
    }

    assert(!is_wait_2pl());

    if (no_successor_response())
    {
      Logging::cout() << "No successor, so releasing the cown" << Logging::endl;
      auto slot_addr = this;
      if (cown()->last_slot.compare_exchange_strong(
            slot_addr, nullptr, std::memory_order_acq_rel))
      {
        yield();

        if (is_read_only())
        {
          drop_read();
        }

        yield();
        // Release cown as this will be set by the new thread joining the
        // queue.
        Logging::cout() << "CAS Success No more work for cown "
                        << Logging::endl;
        shared::release(cown());
        return;
      }

      // If we failed, then the another thread is extending the chain
      while (no_successor_response())
      {
        Systematic::yield_until([this]() { return !no_successor_response(); });
        Aal::pause();
      }
    }

    if (is_read_only())
    {
      Logging::cout() << "Completing read " << *this << Logging::endl;
      drop_read();
      return;
    }

    if (!is_next_slot_read_only())
    {
      Logging::cout() << "Writer waking up next writer cown next slot "
                      << *next_behaviour() << Logging::endl;
      next_behaviour()->resolve();
      return;
    }

    // Final case, we are a writer and waking up at least one reader.

    bool first_reader = cown()->read_ref_count.add_read();
    assert(first_reader);
    snmalloc::UNUSED(first_reader);

    yield();

    Logging::cout() << "Writer waking up next reader and acquiring "
                       "reference count for first reader."
                    << *this << "next slot " << *next_slot() << Logging::endl;

    Cown::acquire(cown());
    yield();

    bool writer_at_end = false;

    Slot* curr_slot = next_slot();
    // First reader is already added in with add_read above, so start at 0.
    size_t count = 0;
    while (true)
    {
      auto status = curr_slot->set_read_available_contended();
      if (status)
      {
        // We marked successfully read-available, so we can stop.
        break;
      }

      if (!curr_slot->is_next_slot_read_only())
      {
        Logging::cout() << "Writer waking up chain with next writer at end "
                        << *curr_slot << Logging::endl;
        // Found a writer, so we can stop.
        writer_at_end = true;
        break;
      }

      yield();
      curr_slot = curr_slot->next_slot();
      count++;
    }

    // Add read count for readers.
    cown()->read_ref_count.add_read(count);

    yield();

    if (writer_at_end)
    {
      auto result = cown()->read_ref_count.try_write();
      // There should definitely be at least one reader in the chain.
      assert(!result);
      snmalloc::UNUSED(result);
      yield();
      cown()->next_writer = curr_slot->next_behaviour();
      yield();
    }

    auto last_slot = curr_slot;
    curr_slot = next_slot();
    while (curr_slot != last_slot)
    {
      auto next = curr_slot->next_slot();
      curr_slot->get_behaviour()->resolve(1, false);
      curr_slot = next;
    }
    last_slot->get_behaviour()->resolve(1, false);
  }
} // namespace verona::rt
