// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

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

    /// Progress and synchronization between the threads.
    //  These counters represent progress on a CPU core, not necessarily on
    //  the core's queue. This is necessary to take into account core-stealing
    //  to avoid spawning many threads on a core hogged by a long running
    //  behaviour but with an empty cown queue.
    std::atomic<std::size_t> servicing_threads{0};

    SchedulerStats stats;

  public:
    Core() : q{} {}
  };
}
