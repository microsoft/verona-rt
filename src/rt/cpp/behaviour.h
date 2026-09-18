// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../boc/behaviourcore.h"
#include "../object/verona_object_model.h"
#include "behaviour_rerun.h"

namespace verona::rt
{
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

  /**
   * This class provides the full `when` functionality.  It
   * provides the closure and lifetime management for the class.
   */
  class Behaviour : public BehaviourCore
  {
    template<typename Be>
    static void invoke(Work* work) noexcept
    {
      // Dispatch to the body of the behaviour.
      BehaviourCore* b = BehaviourCore::from_work(work);
      Be* body = b->get_body<Be>();

      (*body)();
      if (take_behaviour_rerun_request())
      {
        Scheduler::schedule(work);
        return;
      }
      // Dealloc behaviour
      body->~Be();

      BehaviourCore::finished(work);
    }

  public:
    template<typename Be>
    static BehaviourCore::Construction make(size_t count, Be&& f)
    {
      auto construction =
        BehaviourCore::make(count, sizeof(Be), alignof(Be), invoke<Be>);
      new (construction.body) Be(std::forward<Be>(f));

      return construction;
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
      Request* requests = (Request*)heap::alloc(count * sizeof(Request));

      for (size_t i = 0; i < count; ++i)
      {
        requests[i] = Request::write(cowns[i]);
        if constexpr (transfer == YesTransfer)
        {
          requests[i].mark_move();
        }
      }

      schedule<Be>(count, requests, std::forward<Be>(f));

      heap::dealloc(requests);
    }

    /**
     * Prepare a multimessage
     **/

    template<typename Be>
    static Behaviour*
    prepare_to_schedule(size_t count, Request* requests, Be&& f)
    {
      auto construction = Behaviour::make<Be>(count, std::forward<Be>(f));

      Logging::cout() << "Created behaviour " << construction.behaviour
                      << " with ";
      for (size_t i = 0; i < count; i++)
      {
        Logging::cout() << requests[i].cown()
                        << (requests[i].is_read() ? "-R, " : "-RW, ");
        BehaviourCore::initialise_request(
          construction,
          i,
          requests[i].cown(),
          requests[i].is_read() ? AccessMode::Read : AccessMode::Write,
          requests[i].is_move() ? Ownership::Transferred : Ownership::Borrowed);
      }
      Logging::cout() << Logging::endl;

      return (Behaviour*)BehaviourCore::finish_construction(construction);
    }

    template<class Be>
    static void schedule(size_t count, Request* requests, Be&& f)
    {
      Logging::cout() << "Schedule behaviour of type: " << typeid(Be).name()
                      << Logging::endl;

      auto* body =
        prepare_to_schedule<Be>(count, requests, std::forward<Be>(f));

      BehaviourCore::schedule(body);
    }
  };
} // namespace verona::rt
