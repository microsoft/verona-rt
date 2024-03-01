// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../sched/behaviour.h"
#include "../sched/notification.h"
#include "vobject.h"

namespace verona::rt
{
  template<TransferOwnership transfer = NoTransfer, typename T>
  static void schedule_lambda(Cown* c, T&& f)
  {
    Behaviour::schedule<transfer>(c, std::forward<T>(f));
  }

  template<TransferOwnership transfer = NoTransfer, typename T>
  static void schedule_lambda(size_t count, Cown** cowns, T&& f)
  {
    Behaviour::schedule<transfer>(count, cowns, std::forward<T>(f));
  }

  template<TransferOwnership transfer = NoTransfer, typename T>
  static void schedule_lambda(size_t count, Request* requests, T&& f)
  {
    Behaviour::schedule<transfer>(count, requests, std::forward<T>(f));
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

  // TODO super minimal version initially, just to get the tests working.
  // Should be expanded to cover multiple cowns.
  template<typename T>
  inline Notification* make_notification(Cown* cown, T&& f)
  {
    Request requests[] = {Request::write(cown)};
    return Notification::make<T>(1, requests, std::forward<T>(f));
  }

} // namespace verona::rt
