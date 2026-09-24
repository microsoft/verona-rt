// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

/**
 * Preserves the message topology exercised by the former cowngc stress tests
 * without their obsolete global leak-detector machinery.
 *
 * A configurable number of ring cowns form a serial forwarding chain. Each
 * ring behaviour schedules the behaviour on the next cown, continuing for a
 * configurable number of complete rounds. Every ring behaviour also fans out
 * to a configurable number of persistent child cowns. Each child behaviour
 * then sends one message to a shared sink:
 *
 *   ring node -> every child -> shared sink
 *
 * A one-node ring covers self-rescheduling. Larger rings cover short and long
 * forwarding cycles. The target index wraps around the ring, so every cown
 * receives exactly one message per round. Additional rounds increase the work
 * and the opportunities for child and sink queues to build a backlog. The
 * children create fan-out and independent queues. The sink creates fan-in from
 * all child queues.
 *
 * The registered systematic tests use one, two, three, and six ring cowns for
 * five rounds on one, two, and four cores. The registered concurrent tests use
 * one, two, three, and eleven ring cowns for ten rounds on each core count in
 * CON_CORES. Both matrices use three children. Command-line options allow other
 * ring, child, and round counts when investigating a failure.
 *
 * When the sink receives the final message, it schedules one when that acquires
 * the entire ring, every child, and the sink atomically. That behaviour checks
 * the count on every cown before dropping the Topology's owning cown_ptrs. The
 * scheduler-termination callback then checks the destructor counters.
 */
namespace
{
  size_t cores;
  size_t configured_ring_size;
  size_t configured_child_count;
  std::atomic<size_t> finished_threads{0};
  std::atomic<size_t> ring_destroyed{0};
  std::atomic<size_t> children_destroyed{0};
  std::atomic<size_t> sink_destroyed{0};

  struct RingNode
  {
    size_t messages = 0;

    ~RingNode()
    {
      ring_destroyed++;
    }
  };

  struct Child
  {
    size_t messages = 0;

    ~Child()
    {
      children_destroyed++;
    }
  };

  struct Sink
  {
    size_t messages = 0;

    ~Sink()
    {
      sink_destroyed++;
    }
  };

  struct Topology
  {
    std::vector<cown_ptr<RingNode>> ring;
    std::vector<cown_ptr<Child>> children;
    cown_ptr<Sink> sink;
    size_t rounds;
    size_t total_ring_messages;
    size_t total_child_messages;

    Topology(size_t ring_size, size_t child_count, size_t rounds)
    : rounds(rounds),
      total_ring_messages(ring_size * rounds),
      total_child_messages(total_ring_messages * child_count)
    {
      ring.reserve(ring_size);
      children.reserve(child_count);
    }
  };

  void schedule_ring_message(
    const std::shared_ptr<Topology>& topology, size_t message);

  void schedule_final_check(const std::shared_ptr<Topology>& topology)
  {
    cown_array<RingNode> ring(topology->ring.data(), topology->ring.size());
    cown_array<Child> children(
      topology->children.data(), topology->children.size());

    when(std::move(ring), std::move(children), topology->sink)
      << [topology](
           acquired_cown_span<RingNode> ring,
           acquired_cown_span<Child> children,
           acquired_cown<Sink> sink) {
           for (size_t i = 0; i < ring.length(); i++)
             check(ring[i]->messages == topology->rounds);

           for (size_t i = 0; i < children.length(); i++)
             check(children[i]->messages == topology->total_ring_messages);

           check(sink->messages == topology->total_child_messages);

           topology->ring.clear();
           topology->children.clear();
           topology->sink = nullptr;
         };
  }

  void schedule_child_message(
    const std::shared_ptr<Topology>& topology, const cown_ptr<Child>& target)
  {
    when(target) << [topology](acquired_cown<Child> child) {
      child->messages++;

      when(topology->sink) << [topology](acquired_cown<Sink> sink) {
        sink->messages++;
        if (sink->messages == topology->total_child_messages)
          schedule_final_check(topology);
      };
    };
  }

  void schedule_ring_message(
    const std::shared_ptr<Topology>& topology, size_t message)
  {
    auto target = topology->ring[message % topology->ring.size()];
    when(std::move(target))
      << [topology, message](acquired_cown<RingNode> node) {
           node->messages++;

           for (const auto& child : topology->children)
             schedule_child_message(topology, child);

           if (message + 1 < topology->total_ring_messages)
             schedule_ring_message(topology, message + 1);
         };
  }

  void test_body(size_t ring_size, size_t child_count, size_t rounds)
  {
    check(ring_size > 0);
    check(child_count > 0);
    check(rounds > 0);

    ring_destroyed = 0;
    children_destroyed = 0;
    sink_destroyed = 0;

    auto topology = std::make_shared<Topology>(ring_size, child_count, rounds);
    for (size_t i = 0; i < ring_size; i++)
      topology->ring.push_back(make_cown<RingNode>());
    for (size_t i = 0; i < child_count; i++)
      topology->children.push_back(make_cown<Child>());
    topology->sink = make_cown<Sink>();

    schedule_ring_message(topology, 0);
  }

  void finish()
  {
    if (finished_threads.fetch_add(1) != cores - 1)
      return;

    check(ring_destroyed == configured_ring_size);
    check(children_destroyed == configured_child_count);
    check(sink_destroyed == 1);

    finished_threads = 0;
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);
  auto ring_size = harness.opt.is<size_t>("--ring", 4);
  auto child_count = harness.opt.is<size_t>("--children", 3);
  auto rounds = harness.opt.is<size_t>("--rounds", 5);

  cores = harness.cores;
  configured_ring_size = ring_size;
  configured_child_count = child_count;
  harness.run_at_termination = finish;
  harness.run(test_body, ring_size, child_count, rounds);
}
