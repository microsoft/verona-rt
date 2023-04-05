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
    std::atomic<size_t> pause_count{0};
    std::atomic<size_t> unpause_count{0};
    std::atomic<size_t> lifo_count{0};
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
      pause_count += that.pause_count;
      unpause_count += that.unpause_count;
      lifo_count += that.lifo_count;
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
            << "LIFO"
            << "Pause"
            << "Unpause"
            << "Cown count";

        for (size_t i = 0; i < behaviour_count.size(); i++)
          csv << i;
            
        csv << std::endl;
      }

      csv << "SchedulerStats" << get_tag() << dumpid << steal_count << lifo_count
          << pause_count << unpause_count << cown_count;
          
      for (size_t i = 0; i < behaviour_count.size(); i++)
        csv << behaviour_count[i];    
      csv << std::endl;

      steal_count = 0;
      pause_count = 0;
      unpause_count = 0;
      lifo_count = 0;
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
