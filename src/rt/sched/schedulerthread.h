// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../debug/systematic.h"
#include "core.h"
#include "ds/dllist.h"
#include "ds/hashmap.h"
#include "mpmcq.h"
#include "object/object.h"
#include "schedulerlist.h"
#include "schedulerstats.h"
#include "threadpool.h"

#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  /**
   * There is typically one scheduler thread pinned to each physical CPU core.
   * Each scheduler thread is responsible for running cowns in its queue and
   * periodically stealing cowns from the queues of other scheduler threads.
   * This periodic work stealing is done to fairly distribute work across the
   * available scheduler threads. The period of work stealing for fairness is
   * determined by a single token cown that will be dequeued once all cowns
   * before it have been run. The removal of the token cown from the queue
   * occurs at a rate inversely proportional to the amount of cowns pending work
   * on that thread. A scheduler thread will enqueue a new token, if its
   * previous one has been dequeued or stolen, once more work is scheduled on
   * the scheduler thread.
   */
  class SchedulerThread
  {
  public:
    /// Friendly thread identifier for logging information.
    size_t systematic_id = 0;

  private:
    using Scheduler = ThreadPool<SchedulerThread>;
    friend Scheduler;
    friend DLList<SchedulerThread>;
    friend SchedulerList<SchedulerThread>;

    template<typename Owner>
    friend class Noticeboard;

    static constexpr uint64_t TSC_QUIESCENCE_TIMEOUT = 1'000'000;

    Core* core = nullptr;
#ifdef USE_SYSTEMATIC_TESTING
    friend class ThreadSyncSystematic<SchedulerThread>;
    Systematic::Local* local_systematic{nullptr};
#else
    friend class ThreadSync<SchedulerThread>;
    LocalSync local_sync{};
#endif

    Alloc* alloc = nullptr;
    Core* victim = nullptr;

    /// Local work item to avoid overhead of synchronisation
    /// on scheduler queue.
    Work* next_work = nullptr;

    bool running = true;

    /// SchedulerList pointers.
    SchedulerThread* prev = nullptr;
    SchedulerThread* next = nullptr;

    void (*run_at_termination)(void) = nullptr;

    SchedulerThread()
    {
      Logging::cout() << "Scheduler Thread created" << Logging::endl;
    }

    ~SchedulerThread() {}

    void set_core(Core* core)
    {
      this->core = core;
    }

    inline void stop()
    {
      running = false;
    }

    inline void schedule_fifo(Work* w)
    {
      Logging::cout() << "Enqueue work " << w << Logging::endl;

      // If we already have a work item then we need to enqueue it.
      return_next_work();

      // Save work item locally, this is used for batching.
      next_work = w;
    }

    static inline void schedule_lifo(Core* c, Work* w)
    {
      // A lifo scheduled cown is coming from an external source, such as
      // asynchronous I/O.
      Logging::cout() << "LIFO scheduling work " << w << " onto " << c->affinity
                      << Logging::endl;
      c->q.enqueue_front(w);
      Logging::cout() << "LIFO scheduled work " << w << " onto " << c->affinity
                      << Logging::endl;

      c->stats.lifo();

      if (Scheduler::get().unpause())
        c->stats.unpause();
    }

    template<typename... Args>
    static void run(SchedulerThread* t, void (*startup)(Args...), Args... args)
    {
      t->run_inner(startup, args...);
    }

    void return_next_work()
    {
      if (next_work != nullptr)
      {
        core->q.enqueue(next_work);
        next_work = nullptr;
        if (Scheduler::get().unpause())
          core->stats.unpause();
      }
    }

    static constexpr size_t BATCH_SIZE = 100;
    Work* get_work(size_t& batch)
    {
      // Check if we have a thread-local work item to use that is not subject
      // to work stealing.  This is batched, and should not happen more than
      // BATCH_SIZE times in a row.
      if (next_work != nullptr && batch != 0)
      {
        batch--;
        return std::exchange(next_work, nullptr);
      }

      batch = BATCH_SIZE;

      if (core->should_steal_for_fairness)
      {
        // Can race with other threads on the same core.
        // This is a heuristic, so we don't care.
        core->should_steal_for_fairness = false;
        auto work = try_steal();
        if (work != nullptr)
        {
          return_next_work();
          return work;
        }
      }

      auto work = core->q.dequeue(*alloc);
      if (work != nullptr)
      {
        return_next_work();
        return work;
      }

      // Our queue is effectively empty, so this is like receiving a token,
      // try a steal.
      work = try_steal();
      if (work != nullptr)
      {
        return_next_work();
        return work;
      }

      if (next_work != nullptr)
      {
        return std::exchange(next_work, nullptr);
      }

      return steal();
    }

    /**
     * Startup is supplied to initialise thread local state before the runtime
     * starts.
     *
     * This is used for initialising the interpreters per-thread data-structures
     **/
    template<typename... Args>
    void run_inner(void (*startup)(Args...), Args... args)
    {
      startup(args...);

      Scheduler::local() = this;
      alloc = &ThreadAlloc::get();
      assert(core != nullptr);
      victim = core->next;
      core->servicing_threads++;

#ifdef USE_SYSTEMATIC_TESTING
      Systematic::attach_systematic_thread(local_systematic);
#endif
      size_t batch = BATCH_SIZE;
      Work* work;
      while ((work = get_work(batch)))
      {
        Logging::cout() << "Schedule work " << work << Logging::endl;

        work->run();

        yield();
      }

      if (core != nullptr)
      {
        auto val = core->servicing_threads.fetch_sub(1);
        if (val == 1)
        {
          Logging::cout() << "Destroying core " << core->affinity
                          << Logging::endl;
          core->q.destroy(*alloc);
        }
      }

      Systematic::finished_thread();
      if (run_at_termination)
        run_at_termination();

      // Reset the local thread pointer as this physical thread could be reused
      // for a different SchedulerThread later.
      Scheduler::local() = nullptr;
    }

    Work* try_steal()
    {
      Work* work = nullptr;
      // Try to steal from the victim thread.
      if (victim != core)
      {
        work = victim->q.dequeue(*alloc);

        if (work != nullptr)
        {
          // stats.steal();
          Logging::cout() << "Fast-steal work " << work << " from "
                          << victim->affinity << Logging::endl;
        }
      }

      // Move to the next victim thread.
      victim = victim->next;

      return work;
    }

    Work* steal()
    {
      uint64_t tsc = Aal::tick();
      Work* work;

      while (running)
      {
        yield();

        // Check if some other thread has pushed work on our queue.
        work = core->q.dequeue(*alloc);

        if (work != nullptr)
          return work;

        // Try to steal from the victim thread.
        if (victim != core)
        {
          work = victim->q.dequeue(*alloc);

          if (work != nullptr)
          {
            core->stats.steal();
            Logging::cout() << "Stole work " << work << " from "
                            << victim->affinity << Logging::endl;
            return work;
          }
        }

        // We were unable to steal, move to the next victim thread.
        victim = victim->next;

#ifdef USE_SYSTEMATIC_TESTING
        // Only try to pause with 1/(2^5) probability
        UNUSED(tsc);
        if (!Systematic::coin(5))
        {
          yield();
          continue;
        }
#else
        // Wait until a minimum timeout has passed.
        uint64_t tsc2 = Aal::tick();
        if ((tsc2 - tsc) < TSC_QUIESCENCE_TIMEOUT)
        {
          Aal::pause();
          continue;
        }
#endif

        // We've been spinning looking for work for some time. While paused,
        // our running flag may be set to false, in which case we terminate.
        if (Scheduler::get().pause())
          core->stats.pause();
      }

      return nullptr;
    }

    SchedulerStats& get_stats()
    {
      if (core != nullptr)
        return core->stats;

      return SchedulerStats::get_global();
    }
  };

  using Scheduler = ThreadPool<SchedulerThread>;
} // namespace verona::rt

namespace Logging
{
  using namespace verona::rt;

  inline std::string get_systematic_id()
  {
#if defined(USE_SYSTEMATIC_TESTING) || defined(USE_FLIGHT_RECORDER)
    static std::atomic<size_t> external_id_source = 1;
    static thread_local size_t external_id = 0;
    auto s = verona::rt::Scheduler::local();
    if (s != nullptr)
    {
      std::stringstream ss;
      auto offset = static_cast<int>(s->systematic_id % 9);
      if (offset != 0)
        ss << std::setw(offset) << " ";
      ss << s->systematic_id;
      ss << std::setw(9 - offset) << " ";
      return ss.str();
    }
    if (external_id == 0)
    {
      auto e = external_id_source.fetch_add(1);
      external_id = e;
    }
    std::stringstream ss;
    bool short_id = external_id <= 26;
    unsigned char spaces = short_id ? 9 : 8;
    // Modulo guarantees that this fits into the same type as spaces.
    decltype(spaces) offset =
      static_cast<decltype(spaces)>((external_id - 1) % spaces);
    if (offset != 0)
      ss << std::setw(spaces - offset) << " ";
    if (short_id)
      ss << (char)('a' + (external_id - 1));
    else
      ss << 'E' << (external_id - 26);
    ss << std::setw(offset) << " ";
    return ss.str();
#else
    return "";
#endif
  }
}
