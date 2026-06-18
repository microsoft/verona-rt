// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <iostream>
#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  using namespace snmalloc;
  class CSVStream
  {
  private:
    std::ostream& out;
    bool first = true;

  public:
    CSVStream(std::ostream& o) : out(o) {}

    template<typename T>
    CSVStream& operator<<(T&& t)
    {
      if (!first)
        out << ",";
      first = false;
      out << std::forward<T>(t);
      return *this;
    }

    CSVStream& operator<<(std::ostream& (*f)(std::ostream&))
    {
      out << f;
      first = true;
      return *this;
    }
  };

  class SchedulerStats
  {
  private:
#ifdef USE_SCHED_STATS
    std::atomic<size_t> steal_count{0};
    std::atomic<size_t> steal_attempt_count{0};
    std::atomic<size_t> pause_count{0};
    std::atomic<size_t> pause_aborted_check_for_work_count{0};
    std::atomic<size_t> pause_aborted_race_count{0};
    std::atomic<size_t> unpause_count{0};
    std::atomic<size_t> lifo_count{0};
    std::atomic<size_t> sleep_count{0};
    std::atomic<size_t> wake_count{0};
    std::array<std::atomic<size_t>, 16> behaviour_count{};
    std::atomic<size_t> cown_count{0};
#endif
  public:
    ~SchedulerStats()
#ifdef USE_SCHED_STATS
    {
      static snmalloc::FlagWord lock;
      auto& global = get_global();

      if (this != &global)
      {
        FlagLock f(lock);
        global.add(*this);
      }
    }
#else
      = default;
#endif

    void steal()
    {
#ifdef USE_SCHED_STATS
      steal_count++;
#endif
    }

    void pause()
    {
#ifdef USE_SCHED_STATS
      pause_count++;
#endif
    }

    void unpause()
    {
#ifdef USE_SCHED_STATS
      unpause_count++;
#endif
    }

    void lifo()
    {
#ifdef USE_SCHED_STATS
      lifo_count++;
#endif
    }

    /**
     * Counts one entry into SleepHandle::sleep() i.e. one futex_wait syscall.
     */
    static void record_sleep()
    {
#ifdef USE_SCHED_STATS
      get_global().sleep_count++;
#endif
    }

    /**
     * Counts one entry into SleepHandle::wake() i.e. one futex_wake syscall.
     */
    static void record_wake()
    {
#ifdef USE_SCHED_STATS
      get_global().wake_count++;
#endif
    }

    /**
     * Counts one entry into SchedulerThread::steal() (own queue was empty).
     */
    void steal_attempt()
    {
#ifdef USE_SCHED_STATS
      steal_attempt_count++;
#endif
    }

    /**
     * Counts one occasion where pause() aborted because
     * check_for_work() found work after pause_epoch was bumped.
     */
    void pause_aborted_check_for_work()
    {
#ifdef USE_SCHED_STATS
      pause_aborted_check_for_work_count++;
#endif
    }

    /**
     * Counts one occasion where pause() aborted because the
     * unpause_epoch changed under the scheduler lock.
     */
    void pause_aborted_race()
    {
#ifdef USE_SCHED_STATS
      pause_aborted_race_count++;
#endif
    }

    void behaviour(size_t cowns)
    {
      UNUSED(cowns);
#ifdef USE_SCHED_STATS
      if (cowns < behaviour_count.size())
        behaviour_count[cowns]++;
      else
        behaviour_count.back()++;
#endif
    }

    void cown()
    {
#ifdef USE_SCHED_STATS
      cown_count++;
#endif
    }

    void add(SchedulerStats& that)
    {
      UNUSED(that);

#ifdef USE_SCHED_STATS
      steal_count += that.steal_count;
      steal_attempt_count += that.steal_attempt_count;
      pause_count += that.pause_count;
      pause_aborted_check_for_work_count +=
        that.pause_aborted_check_for_work_count;
      pause_aborted_race_count += that.pause_aborted_race_count;
      unpause_count += that.unpause_count;
      lifo_count += that.lifo_count;
      sleep_count += that.sleep_count;
      wake_count += that.wake_count;
      cown_count += that.cown_count;

      for (size_t i = 0; i < behaviour_count.size(); i++)
        behaviour_count[i] += that.behaviour_count[i];
#endif
    }

    void dump(std::ostream& o, uint64_t dumpid = 0)
    {
      UNUSED(o);
      UNUSED(dumpid);

#ifdef USE_SCHED_STATS
      CSVStream csv(o);

      if (dumpid == 0)
      {
        // Output headers for initial dump
        // Keep in sync with data dump
        csv << "SchedulerStats"
            << "Tag"
            << "DumpID"
            << "Steal"
            << "StealAttempt"
            << "LIFO"
            << "Pause"
            << "PauseAbortCheckForWork"
            << "PauseAbortRace"
            << "Unpause"
            << "Sleep"
            << "Wake"
            << "Cown count";

        for (size_t i = 0; i < behaviour_count.size(); i++)
          csv << i;

        csv << std::endl;
      }

      csv << "SchedulerStats" << get_tag() << dumpid << steal_count
          << steal_attempt_count << lifo_count << pause_count
          << pause_aborted_check_for_work_count << pause_aborted_race_count
          << unpause_count << sleep_count << wake_count << cown_count;

      for (size_t i = 0; i < behaviour_count.size(); i++)
        csv << behaviour_count[i];
      csv << std::endl;

      steal_count = 0;
      steal_attempt_count = 0;
      pause_count = 0;
      pause_aborted_check_for_work_count = 0;
      pause_aborted_race_count = 0;
      unpause_count = 0;
      lifo_count = 0;
      sleep_count = 0;
      wake_count = 0;
      cown_count = 0;

      for (size_t i = 0; i < behaviour_count.size(); i++)
        behaviour_count[i] = 0;
#endif
    }

    static void dump_global(std::ostream& o, uint64_t dumpid)
    {
#ifdef USE_SCHED_STATS
      get_global().dump(o, dumpid);
#endif
    }

    static SchedulerStats& get_global()
    {
      static SchedulerStats global;
      return global;
    }

    static std::string& get_tag()
    {
      static std::string tag = "";
      return tag;
    }
  };
} // namespace verona::rt
