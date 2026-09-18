// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../boc/behaviourcore.h"
#include "cown.h"

namespace verona::rt
{
  /**
   * Adapter between the BoC protocol and the existing Verona object model.
   */
  struct VeronaObjectModel
  {
    using Cown = verona::rt::Cown;

    static CownSchedulerState<VeronaObjectModel>&
    get_cown_scheduler_state(Cown& cown) noexcept
    {
      return cown.scheduler_state;
    }

    static void acquire(Cown& cown) noexcept
    {
      Shared::acquire(&cown);
    }

    static void release(Cown& cown) noexcept
    {
      Shared::release(&cown);
    }

    static uintptr_t get_cown_identity(const Cown& cown) noexcept
    {
      return cown.id();
    }
  };

  using BehaviourCore = boc::BehaviourCore<VeronaObjectModel>;
  using Slot = boc::Slot<VeronaObjectModel>;
}
