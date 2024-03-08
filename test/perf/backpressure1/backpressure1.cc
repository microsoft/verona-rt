// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * This tests a simple scenario for backpressure where many individual `Sender`
 * cowns send messages to a single set of `Receiver` cowns. The `Recevier` cowns
 * may be placed behind a chain of `Proxy` cowns to test backpressure
 * propagation.
 *
 * Without backpressure, the receivers would have their queues grow at a much
 * higher rate than they could process the messages. The muted proxies may also
 * experience similar queue growth if the backpressure is not corretly
 * propagated from the receiver set.
 */

#include "cpp/when.h"
#include "debug/harness.h"
#include "debug/log.h"
#include "test/opt.h"
#include "verona.h"

#include <chrono>

using namespace verona::rt;
using namespace verona::cpp;
using timer = std::chrono::high_resolution_clock;

struct Receiver;
struct Proxy;
static std::vector<Receiver*> receiver_set;
static std::vector<Proxy*> proxy_chain;

struct Receiver : public VCown<Receiver>
{
  static constexpr size_t report_count = 1'000'000;
  size_t msgs = 0;
  timer::time_point prev = timer::now();
};

struct Receive
{
  void operator()()
  {
    auto& r = *receiver_set[0];
    r.msgs++;
    if ((r.msgs % Receiver::report_count) != 0)
      return;

    const auto now = timer::now();
    const auto t =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - r.prev);
    logger::cout() << Receiver::report_count << " messages received in "
                   << t.count() << "ms" << std::endl;
    r.prev = now;
  }
};

struct Proxy : public VCown<Proxy>
{
  size_t index;

  Proxy(size_t index_) : index(index_) {}

  void trace(ObjectStack& st) const
  {
    if (this != proxy_chain.back())
    {
      st.push(proxy_chain[index + 1]);
      return;
    }

    for (auto* r : receiver_set)
      st.push(r);
  }
};

struct Forward
{
  Proxy* proxy;

  Forward(Proxy* proxy_) : proxy(proxy_) {}

  void operator()()
  {
    if (proxy != proxy_chain.back())
    {
      auto* next = proxy_chain[proxy->index + 1];
      schedule_lambda(next, Forward(next));
      return;
    }

    schedule_lambda(
      receiver_set.size(), (Cown**)receiver_set.data(), Receive());
  }
};

struct Sender : public VCown<Sender>
{
  using clk = std::chrono::steady_clock;

  clk::time_point start = clk::now();
  std::chrono::milliseconds duration;

  Sender(std::chrono::milliseconds duration_) : duration(duration_) {}

  void trace(ObjectStack& st) const
  {
    if (proxy_chain.size() > 0)
    {
      st.push(proxy_chain[0]);
      return;
    }

    for (auto* r : receiver_set)
      st.push(r);
  }
};

struct Send
{
  Sender* s;

  Send(Sender* s_) : s(s_) {}

  void operator()()
  {
    if (proxy_chain.size() > 0)
      schedule_lambda(proxy_chain[0], Forward(proxy_chain[0]));
    else
      schedule_lambda(
        receiver_set.size(), (Cown**)receiver_set.data(), Receive());

    if ((Sender::clk::now() - s->start) < s->duration)
      schedule_lambda(s, Send(s));
  }
};

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  auto senders = harness.opt.is<size_t>("--senders", 100);
  auto receivers = harness.opt.is<size_t>("--receivers", 1);
  auto proxies = harness.opt.is<size_t>("--proxies", 0);
  auto duration = harness.opt.is<size_t>("--duration", 10'000);

  harness.run([senders, receivers, proxies, duration, &harness]() {
    Alloc& alloc = ThreadAlloc::get();

    for (size_t r = 0; r < receivers; r++)
      receiver_set.push_back(new (alloc) Receiver);

    for (size_t p = 0; p < proxies; p++)
      proxy_chain.push_back(new (alloc) Proxy(p));

    auto e = make_cown<int>();
    when(e) << [](auto) {
      Logging::cout() << "Add external event source" << std::endl;
      Scheduler::add_external_event_source();
    };

    harness.external_thread([=]() {
      Alloc& alloc = ThreadAlloc::get();
      for (size_t i = 0; i < senders; i++)
      {
        if (proxy_chain.size() > 0)
        {
          Cown::acquire(proxy_chain[0]);
        }
        else
        {
          for (auto* r : receiver_set)
            Cown::acquire(r);
        }

        auto* s = new (alloc) Sender(std::chrono::milliseconds(duration));
        schedule_lambda<YesTransfer>(s, Send(s));
      }

      if (proxy_chain.size() > 0)
      {
        Cown::release(alloc, proxy_chain[0]);
      }
      else
      {
        for (auto* r : receiver_set)
          Cown::release(alloc, r);
      }

      when(e) << [](auto) {
        Logging::cout() << "Remove external event source" << std::endl;
        Scheduler::remove_external_event_source();
      };
    });
  });
}
