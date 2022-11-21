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

  struct Behaviour;

  struct Slot
  {
    Cown* cown;
    /**
     * Possible vales:
     *   0 - Wait
     *   1 - Ready
     *   Behaviour* - Next write
     *
     * TODO
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

    Behaviour* get_behaviour()
    {
      return (Behaviour*)status.load(std::memory_order_acquire);
    }

    void set_behaviour(Behaviour* b)
    {
      status.store((uintptr_t)b, std::memory_order_release);
    }

    void release();
  };

  struct Behaviour
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
     * slots. This is because the two phase locking needs to complete before we
     * can execute the behaviour.
     */
    Behaviour(size_t count) : exec_count_down(count + 1), count(count) {}

    Work* as_work()
    {
      return pointer_offset_signed<Work>(this, -sizeof(Work));
    }

    /**
     * Remove one from the exec_count_down.
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

    template<typename Be>
    static void invoke(Work* work)
    {
      // Dispatch to the body of the behaviour.
      Behaviour* behaviour = pointer_offset<Behaviour>(work, sizeof(Work));
      Slot* slots = pointer_offset<Slot>(behaviour, sizeof(Behaviour));
      Be* body = pointer_offset<Be>(slots, sizeof(Slot) * behaviour->count);
      (*body)();

      // Behaviour is done, we can resolve successors.
      for (size_t i = 0; i < behaviour->count; i++)
      {
        slots[i].release();
      }

      // Dealloc behaviour
      body->~Be();
      work->dealloc();
    }

    // TODO: When C++ 20 move to span.
    Slot* get_slots()
    {
      return pointer_offset<Slot>(this, sizeof(Behaviour));
    }

    template<typename Be, typename... Args>
    static Behaviour* make(Alloc& alloc, size_t count, Args... args)
    {
      size_t size =
        sizeof(Work) + sizeof(Behaviour) + (sizeof(Slot) * count) + sizeof(Be);

      // Manual memory layout of the behaviour structure.
      //   | Work | Behaviour | Slot ... Slot | Body |
      void* base = alloc.alloc(size);
      void* base_behaviour = pointer_offset(base, sizeof(Work));
      void* base_slots = pointer_offset(base_behaviour, sizeof(Behaviour));
      void* base_body = pointer_offset(base_slots, sizeof(Slot) * count);

      Work* work = new (base) Work(invoke<Be>);

      Behaviour* behaviour = new (base_behaviour) Behaviour(count);

      new (base_body) Be(std::forward<Args>(args)...);

      // These assertions are basically checking that we won't break any
      // alignment assumptions on Be.  If we add some actual alignment, then
      // this can be improved.
      static_assert(
        sizeof(Slot) % sizeof(void*) == 0,
        "Slot size must be a multiple of pointer size");
      static_assert(
        sizeof(Behaviour) % sizeof(void*) == 0,
        "Behaviour size must be a multiple of pointer size");
      static_assert(
        sizeof(Work) % sizeof(void*) == 0,
        "Work size must be a multiple of pointer size");
      static_assert(
        alignof(Be) <= sizeof(void*), "Alignment not supported, yet!");

      return behaviour;
    }

    template<
      class Behaviour,
      TransferOwnership transfer = NoTransfer,
      typename... Args>
    static void schedule(Cown* cown, Args&&... args)
    {
      schedule<Behaviour, transfer, Args...>(
        1, &cown, std::forward<Args>(args)...);
    }

    template<
      class Behaviour,
      TransferOwnership transfer = NoTransfer,
      typename... Args>
    static void schedule(size_t count, Cown** cowns, Args&&... args)
    {
      // TODO Remove vector allocation here.  This is a temporary fix to
      // as we transition to using Request through the code base.
      auto& alloc = ThreadAlloc::get();
      Request* requests = (Request*)alloc.alloc(count * sizeof(Request));

      for (size_t i = 0; i < count; ++i)
        requests[i] = Request::write(cowns[i]);

      schedule<Behaviour, transfer, Args...>(
        count, requests, std::forward<Args>(args)...);

      alloc.dealloc(requests);
    }

    /**
     * Sends a multi-message to the first cown we want to acquire.
     *
     * Pass `transfer = YesTransfer` as a template argument if the
     * caller is transfering ownership of a reference count on each cown to
     *this method.
     **/
    template<
      class Be,
      TransferOwnership transfer = NoTransfer,
      typename... Args>
    static void schedule(size_t count, Request* requests, Args&&... args)
    {
      Logging::cout() << "Schedule behaviour of type: " << typeid(Be).name()
                      << Logging::endl;

      auto& alloc = ThreadAlloc::get();

      auto body =
        Behaviour::make<Be>(alloc, count, std::forward<Args>(args)...);

      auto* slots = body->get_slots();
      for (size_t i = 0; i < count; i++)
      {
        new (&slots[i]) Slot(requests[i].cown());
      }

      schedule<transfer>(body);
    }

    template<TransferOwnership transfer = NoTransfer>
    static void schedule(Behaviour* body)
    {
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
      // 2PL.
      size_t ec = 1;

      for (size_t i = 0; i < count; i++)
      {
        // Acquire phase.
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
          // Wait for the previous behaviour to finish adding to chains.
          Aal::pause();
          Systematic::yield_until([prev](){ return !prev->is_wait(); });
        }

        if (transfer == YesTransfer)
        {
          Cown::release(ThreadAlloc::get(), cown);
        }

        yield();
        prev->set_behaviour(body);
        yield();
      }

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
        cown::release(ThreadAlloc::get(), cown);
        return;
      }

      yield();

      // If we failed, then the another thread is extending the chain
      while (is_ready())
      {
        Systematic::yield_until([this](){ return !is_ready(); });
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