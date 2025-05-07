// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "behaviour.h"
#include "notification.h"
#include "vobject.h"

namespace verona::rt
{
  template<TransferOwnership transfer = NoTransfer, typename Be>
  static void schedule_lambda(Cown* c, Be&& f)
  {
    Behaviour::schedule<transfer>(c, std::forward<Be>(f));
  }

  template<TransferOwnership transfer = NoTransfer, typename Be>
  static void schedule_lambda(size_t count, Cown** cowns, Be&& f)
  {
    Behaviour::schedule<transfer>(count, cowns, std::forward<Be>(f));
  }

  template<typename Be>
  static void schedule_lambda(Be&& f)
  {
    auto w = Closure::make([f = std::forward<Be>(f)](Work* w) mutable {
      f();
      return true;
    });
    Scheduler::schedule(w);
  }

  // TODO super minimal version initially, just to get the tests working.
  // Should be expanded to cover multiple cowns.
  template<typename Be>
  inline Notification* make_notification(Cown* cown, Be&& f)
  {
    Request requests[] = {Request::write(cown)};
    return Notification::make<Be>(1, requests, std::forward<Be>(f));
  }

} // namespace verona::rt
