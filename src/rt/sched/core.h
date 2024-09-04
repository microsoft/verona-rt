// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "mpmcq.h"
#include "schedulerstats.h"
#include "work.h"

#include <atomic>
#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  class Core
  {
  public:
    size_t affinity = 0;
    MPMCQ<Work> q;
    std::atomic<Core*> next{nullptr};
    std::atomic<bool> should_steal_for_fairness{false};

    /// Progress and synchronization between the threads.
    //  These counters represent progress on a CPU core, not necessarily on
    //  the core's queue. This is necessary to take into account core-stealing
    //  to avoid spawning many threads on a core hogged by a long running
    //  behaviour but with an empty cown queue.
    std::atomic<std::size_t> servicing_threads{0};

    SchedulerStats stats;

    /**
     * @brief Create a token work object.  It is affinitised to the `home`
     * core, and marks that stealing is required, for fairness. Once completed
     * it reschedules itself on the home core.
     */
    Work* create_token_work(Core* home)
    {
      auto w = Closure::make([home](Work* w) {
        home->should_steal_for_fairness = true;
        home->q.enqueue(w);
        return false;
      });
      return w;
    }

  public:
    Core() : q{} {}

    ~Core() {}
  };
}
