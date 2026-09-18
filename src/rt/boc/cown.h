// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

// `ReadRefCount` and the read/write state used by `Cown` are modelled in C#
// at docs/internal/concurrency/modelimpl-readonly/. See CPP_MAPPING.md there
// for the type/method correspondence.

#include "../debug/logging.h"
#include "cown_scheduler_state.h"
#include "shared.h"

#ifdef USE_SYSTEMATIC_TESTING_WEAK_NOTICEBOARDS
#  include "base_noticeboard.h"

#  include <vector>
#endif

namespace verona::rt
{
  class Cown;
  struct VeronaObjectModel;

  /**
   * A cown, or concurrent owner, encapsulates a set of resources that may be
   * accessed by a single (scheduler) thread at a time when writing, or
   * accessed by multiple (scheduler) threads at a time when reading.
   * A cown can only be in one of the following states:
   *   1. Unscheduled
   *   2. Scheduled, in the queue of a single scheduler thread
   *   3. Running with read/write access on a single scheduler thread, and not
   * in the queue of any scheduler thread
   *   4. Running with read access on one or more scheduler threads, and may
   * also be in the queue of one other scheduler thread
   *
   * Once a cown is running, it executes a batch of multi-message behaviours.
   * Each message may either acquire the running cown for participation in a
   * future behaviour, or execute the behaviour if it is the last cown to be
   * acquired.
   * If the running cown is acquired for writing for a future behaviour, it will
   * be descheduled until that behaviour has completed. If the running cown is
   * acquired for reading for a future behaviour, it will not be descheduled. If
   * the running cown is acquired for reading _and_ executing on this thread,
   * the cown will be rescheduled to be picked up by another thread. (it might
   * later return to this thread if this is the last thread to use the cown in
   * read more before a write).
   *
   * The queue and reader/writer state are stored in CownSchedulerState so the
   * BoC protocol can be reused with other object models.
   */
  class Cown : public Shared
  {
  public:
    Cown() {}

  private:
    template<typename T>
    friend class Promise;
    friend VeronaObjectModel;

    template<typename T>
    friend class Noticeboard;

    CownSchedulerState<VeronaObjectModel> scheduler_state;

  public:
    inline friend Logging::SysLog& operator<<(Logging::SysLog& os, Cown& c)
    {
      return os << " Cown: " << &c << c.scheduler_state;
    }

#ifdef USE_SYSTEMATIC_TESTING_WEAK_NOTICEBOARDS
    std::vector<BaseNoticeboard*> noticeboards;

    void flush_all()
    {
      for (auto b : noticeboards)
      {
        b->flush_all();
      }
    }

    void flush_some()
    {
      for (auto b : noticeboards)
      {
        b->flush_some();
      }
    }

    void register_noticeboard(BaseNoticeboard* nb)
    {
      noticeboards.push_back(nb);
    }

#endif
  };

  /**
   * Adapter between the BoC protocol and the existing Verona object model.
   */
  struct VeronaObjectModel
  {
    using Cown = verona::rt::Cown;

    static CownSchedulerState<VeronaObjectModel>&
    get_cown_scheduler_state(Cown& cown)
    {
      return cown.scheduler_state;
    }

    static void acquire(Cown& cown)
    {
      Shared::acquire(&cown);
    }

    static void release(Cown& cown)
    {
      Shared::release(&cown);
    }

    static uintptr_t get_cown_identity(const Cown& cown)
    {
      return cown.id();
    }
  };

  using BehaviourCore = boc::BehaviourCore<VeronaObjectModel>;
  using Slot = boc::Slot<VeronaObjectModel>;
} // namespace verona::rt
