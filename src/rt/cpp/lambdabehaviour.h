// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../sched/behaviour.h"
#include "vobject.h"

namespace verona::rt
{
  template<TransferOwnership transfer = NoTransfer, typename T>
  static void schedule_lambda(Cown* c, T&& f)
  {
    Behaviour::schedule<T, transfer>(c, std::forward<T>(f));
  }

  template<TransferOwnership transfer = NoTransfer, typename T>
  static void schedule_lambda(size_t count, Cown** cowns, T&& f)
  {
    Behaviour::schedule<T, transfer>(count, cowns, std::forward<T>(f));
  }

  template<TransferOwnership transfer = NoTransfer, typename T>
  static void schedule_lambda(size_t count, Request* requests, T&& f)
  {
    Behaviour::schedule<T, transfer>(count, requests, std::forward<T>(f));
  }

  template<typename T>
  static void schedule_lambda(T&& f)
  {
    auto w = Closure::make([f = std::forward<T>(f)](Work* w) mutable {
      f();
      return true;
    });
    Scheduler::schedule(w);
  }

  // TODO Temporary until we actually design notify correctly.
  inline void notify(Cown* p)
  {
    schedule_lambda(p, [p = p]() { p->notified(); });
  }
} // namespace verona::rt
