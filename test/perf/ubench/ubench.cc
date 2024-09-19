// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * A microbenchmark for measuring message passing rates in the Verona runtime.
 * This microbenchmark is adapted from the original message-ubench from the Pony
 * Language examples to include multi-messages.
 *
 * This microbenchmark executes a sequence of intervals that are 1 second long
 * by default. During each interval the `Monitor` cown and a static set of
 * `Pinger` cowns are setup and an initial set of `Ping` messages are sent to
 * the `Pinger`s. When a `Pinger` receives a `Ping` message, the `Pinger` will
 * randomly choose another `Pinger` to forward the `Ping` message. A `Pinger`
 * may randomly choose to include itself in the forwarded `Ping` multi-message
 * along with the selected recipient. By default 5% of `Ping` messages will
 * become these multi-messages.
 */

#include "debug/log.h"
#include "test/opt.h"
#include "test/xoroshiro.h"
#include "verona.h"

#include <chrono>
#include <debug/harness.h>

using namespace std;

namespace sn = snmalloc;
namespace rt = verona::rt;

static rt::Cown** all_cowns = nullptr;
static size_t all_cowns_count = 0;

namespace ubench
{
  struct Pinger : public rt::VCown<Pinger>
  {
    vector<Pinger*>& pingers;
    xoroshiro::p128r32 rng;
    size_t select_mod = 0;
    bool running = false;
    size_t count = 0;

    Pinger(vector<Pinger*>& pingers_, size_t seed, size_t percent_multimessage)
    : pingers(pingers_), rng(seed)
    {
      if (percent_multimessage != 0)
        select_mod = (size_t)((double)100.00 / (double)percent_multimessage);
    }
  };

  struct Monitor : public rt::VCown<Monitor>
  {
    vector<Pinger*>& pingers;
    size_t initial_pings;
    std::chrono::seconds report_interval;
    size_t report_count;
    size_t waiting = 0;
    uint64_t start = 0;

    Monitor(
      vector<Pinger*>& pingers_,
      size_t initial_pings_,
      std::chrono::seconds report_interval_,
      size_t report_count_)
    : pingers(pingers_),
      initial_pings(initial_pings_),
      report_interval(report_interval_),
      report_count(report_count_)
    {}

    void trace(rt::ObjectStack& st) const
    {
      for (auto* p : pingers)
        st.push(p);
    }
  };

  struct Ping
  {
    Pinger* pinger;
    std::array<Pinger*, 2> recipients;

    Ping(Pinger* pinger_) : pinger(pinger_) {}

    void operator()()
    {
      if (!pinger->running)
        return;

      pinger->count++;

      recipients[0] = pinger;
      const bool send_multimessage = (pinger->pingers.size() > 1) &&
        (pinger->select_mod != 0) &&
        ((pinger->rng.next() % pinger->select_mod) == 0);
      if (!send_multimessage)
      {
        schedule_lambda(recipients[0], Ping(recipients[0]));
        return;
      }

      // select another recipient
      do
      {
        recipients[1] =
          pinger->pingers[pinger->rng.next() % pinger->pingers.size()];
      } while (recipients[1] == pinger);

      schedule_lambda(2, (rt::Cown**)recipients.data(), Ping(recipients[0]));
    }
  };

  static void start_timer(Monitor* monitor, std::chrono::milliseconds timeout);

  struct Start
  {
    Monitor* monitor;

    Start(Monitor* monitor_) : monitor(monitor_) {}

    void operator()()
    {
      rt::Scheduler::add_external_event_source();
      for (auto* p : monitor->pingers)
      {
        p->count = 0;
        p->running = true;
        for (size_t i = 0; i < monitor->initial_pings; i++)
          schedule_lambda(p, Ping(p));
      }

      monitor->start = sn::Aal::tick();
      start_timer(monitor, monitor->report_interval);
    }
  };

  struct Report
  {
    Monitor* monitor;

    Report(Monitor* monitor_) : monitor(monitor_) {}

    void trace(rt::ObjectStack& st) const
    {
      st.push(monitor);
    }

    void operator()()
    {
      uint64_t t = sn::Aal::tick() - monitor->start;
      uint64_t sum = 0;
      for (auto* p : monitor->pingers)
        sum += p->count;

      uint64_t rate = (sum * 1'000'000'000) / t;
      logger::cout() << t << " ns, " << rate << " msgs/s" << std::endl;
    }
  };

  struct NotifyStopped
  {
    Monitor* monitor;

    NotifyStopped(Monitor* monitor_) : monitor(monitor_) {}

    void operator()()
    {
      if (--monitor->waiting != 0)
        return;

      schedule_lambda(all_cowns_count, all_cowns, Report(monitor));

      // Drop count, Start will reincrease if more external work is needed.
      rt::Scheduler::remove_external_event_source();

      if (--monitor->report_count != 0)
        schedule_lambda(all_cowns_count, all_cowns, Start(monitor));
      else
        rt::Cown::release(monitor);
    }
  };

  struct StopPinger
  {
    Pinger* pinger;
    Monitor* monitor;

    StopPinger(Pinger* pinger_, Monitor* monitor_)
    : pinger(pinger_), monitor(monitor_)
    {}

    void operator()()
    {
      pinger->running = false;
      schedule_lambda(monitor, NotifyStopped(monitor));
    }
  };

  struct Stop
  {
    Monitor* monitor;

    Stop(Monitor* monitor_) : monitor(monitor_) {}

    void operator()()
    {
      monitor->waiting = monitor->pingers.size();
      for (auto* pinger : monitor->pingers)
        schedule_lambda(pinger, StopPinger(pinger, monitor));
    }
  };

  static void start_timer(Monitor* monitor, std::chrono::milliseconds timeout)
  {
    rt::Cown::acquire(monitor);
    std::thread([=]() mutable {
      std::this_thread::sleep_for(timeout);
      schedule_lambda<rt::YesTransfer>(monitor, Stop(monitor));
    }).detach();
  }
}

using namespace ubench;

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);
  const auto seed = opt.is<size_t>("--seed", 5489);
  const auto cores = opt.is<size_t>("--cores", 4);
  const auto pingers = opt.is<size_t>("--pingers", 8);
  const auto report_interval =
    std::chrono::seconds(opt.is<size_t>("--report_interval", 1));
  const auto report_count = opt.is<size_t>("--report_count", 10);
  const auto initial_pings = opt.is<size_t>("--initial_pings", 5);
  const auto percent_multimessage = opt.is<size_t>("--percent_multimessage", 5);
  check(percent_multimessage <= 100);

  logger::cout() << "cores: " << cores
                 << ", report_interval: " << report_interval.count()
                 << ", pingers: " << pingers
                 << ", initial_pings: " << initial_pings
                 << ", percent_mutlimessage: " << percent_multimessage
                 << std::endl;

#ifdef USE_SYSTEMATIC_TESTING
  Logging::enable_logging();
  Systematic::set_seed(seed);
#else
  UNUSED(seed);
#endif
  auto& sched = rt::Scheduler::get();
  sched.set_fair(true);
  sched.init(cores);

  static vector<Pinger*> pinger_set;
  for (size_t p = 0; p < pingers; p++)
    pinger_set.push_back(
      new Pinger(pinger_set, seed + p, percent_multimessage));

  auto* monitor =
    new Monitor(pinger_set, initial_pings, report_interval, report_count);

  all_cowns_count = pingers + 1;
  all_cowns = (rt::Cown**)heap::alloc(all_cowns_count * sizeof(rt::Cown*));
  memcpy(all_cowns, pinger_set.data(), pinger_set.size() * sizeof(rt::Cown*));
  all_cowns[pinger_set.size()] = monitor;
  schedule_lambda(all_cowns_count, all_cowns, ubench::Start(monitor));

  sched.run();
  heap::dealloc(all_cowns, all_cowns_count * sizeof(rt::Cown*));
  return 0;
}
