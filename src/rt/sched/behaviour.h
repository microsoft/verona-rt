// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "behaviourcore.h"

namespace verona::rt
{
  /**
   * This class provides the full `when` functionality.  It
   * provides the closure and lifetime management for the class.
   */
  class Behaviour : public BehaviourCore
  {
    template<typename Be>
    static void invoke(Work* work)
    {
      // Dispatch to the body of the behaviour.
      BehaviourCore* behaviour = BehaviourCore::from_work(work);
      Be* body = behaviour->get_body<Be>();
      (*body)();

      if (behaviour_rerun())
      {
        behaviour_rerun() = false;
        Scheduler::schedule(work);
        return;
      }

      behaviour->release_all();

      // Dealloc behaviour
      body->~Be();
      work->dealloc();
    }

  public:
    static bool& behaviour_rerun()
    {
      static thread_local bool rerun = false;
      return rerun;
    }

    template<typename Be>
    static Behaviour* make(size_t count, Be&& f)
    {
      auto behaviour_core = BehaviourCore::make(count, invoke<Be>, sizeof(Be));

      new (behaviour_core->get_body()) Be(std::forward<Be>(f));

      // These assertions are basically checking that we won't break any
      // alignment assumptions on Be.  If we add some actual alignment, then
      // this can be improved.
      static_assert(
        alignof(Be) <= sizeof(void*), "Alignment not supported, yet!");

      return (Behaviour*)behaviour_core;
    }

    template<TransferOwnership transfer = NoTransfer, class T>
    static void schedule(Cown* cown, T&& f)
    {
      schedule<transfer, T>(1, &cown, std::forward<T>(f));
    }

    template<TransferOwnership transfer = NoTransfer, class Be>
    static void schedule(size_t count, Cown** cowns, Be&& f)
    {
      // TODO Remove vector allocation here.  This is a temporary fix to
      // as we transition to using Request through the code base.
      auto& alloc = ThreadAlloc::get();
      Request* requests = (Request*)alloc.alloc(count * sizeof(Request));

      for (size_t i = 0; i < count; ++i)
      {
        requests[i] = Request::write(cowns[i]);
        if constexpr (transfer == YesTransfer)
        {
          requests[i].mark_move();
        }
      }

      schedule<Be>(count, requests, std::forward<Be>(f));

      alloc.dealloc(requests);
    }

    /**
     * Prepare a multimessage
     **/

    template<typename Be>
    static Behaviour*
    prepare_to_schedule(size_t count, Request* requests, Be&& f)
    {
      auto body = Behaviour::make<Be>(count, std::forward<Be>(f));

      auto* slots = body->get_slots();
      for (size_t i = 0; i < count; i++)
      {
        auto* s = new (&slots[i]) Slot(requests[i].cown());
        if (requests[i].is_move())
          s->set_move();
        if (requests[i].is_read())
          s->set_read_only();
      }

      return body;
    }

    template<class Be>
    static void schedule(size_t count, Request* requests, Be&& f)
    {
      Logging::cout() << "Schedule behaviour of type: " << typeid(Be).name()
                      << Logging::endl;

      auto* body =
        prepare_to_schedule<Be>(count, requests, std::forward<Be>(f));

      BehaviourCore* arr[] = {body};

      BehaviourCore::schedule_many(arr, 1);
    }
  };
} // namespace verona::rt
