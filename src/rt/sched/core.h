// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "mpmcq.h"
#include "schedulerstats.h"
#include "work.h"
#include "workstealingqueue.h"

#include <atomic>
#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  class Core
  {
  public:
    size_t affinity = 0;
    WorkStealingQueue<4> q;
    std::atomic<Core*> next{nullptr};

    std::atomic<bool> should_steal_for_fairness{true};

    /**
     * @brief Create a token work object.  It is affinitised to the `this`
     * core, and marks that stealing is required, for fairness.
     */
    Work* token_work{Closure::make([this](Work* w) {
      this->should_steal_for_fairness = true;
      // The token work is only deallocated during the destruction of the core.
      // The destructor will run the token work, and return true, so that the
      // closure code will run destructors and deallocate the memory.
      return this->token_work == nullptr;
    })};

    /// Progress and synchronization between the threads.
    //  These counters represent progress on a CPU core, not necessarily on
    //  the core's queue. This is necessary to take into account core-stealing
    //  to avoid spawning many threads on a core hogged by a long running
    //  behaviour but with an empty cown queue.
    std::atomic<std::size_t> servicing_threads{0};

    SchedulerStats stats;

  public:
    Core() : q{} {}

    ~Core()
    {
      auto tw = token_work;
      token_work = nullptr;
      tw->run();
    }
  };
}
