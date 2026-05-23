// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
//
// Regression test for the cascade-walker WaitStatus race in
// `BehaviourCore::release()` → `WakeupReaderChain`.
//
// Trigger shape (chain on a single cown `c`):
//   W1   →   R1   [→   R2 ... → Rn  [→ W2]]
//
// Sequence required to expose the bug:
//   * R1.StartEnqueue runs first and CASes W1.status from STATUS_READY to
//     NextReader(R1). R1's own slot status is still STATUS_WAIT at this
//     point because R1.FinishEnqueue (which publishes R1.status as READY)
//     runs in a later phase of R1's `schedule_many`.
//   * W1's body completes and W1.release() observes W1.status as
//     NextReader(R1), so it dispatches WakeupReaderChain(R1).
//   * The walker reads R1.status BEFORE R1.FinishEnqueue has run, i.e.
//     while it is still STATUS_WAIT.
//
// Without a `while (curr_slot->is_wait_2pl()) yield/pause` spin before
// `curr_slot->set_read_available_contended()` at behaviourcore.h:1370,
// the walker:
//   * fails set_read_available_contended (status != STATUS_READY);
//   * reads is_next_slot_read_only() = ((0x0) & STATUS_READ_FLAG) == false,
//     misclassifying R1 as having a writer successor;
//   * sets writer_at_end = true and cown.next_writer = next_behaviour() =
//     (BehaviourCore*)(0x0 & STATUS_NEXT_SLOT_MASK) = nullptr;
//   * sets the WRITER_WAITING_BIT on read_ref_count via try_write().
// Last-reader drop_read then calls wakeup_next_writer(), which spins
// forever on next_writer == nullptr — a silent deadlock in release
// builds.
//
// In debug builds the assertion `assert(status != STATUS_WAIT)` at
// behaviourcore.h:159 inside set_read_available_contended fires
// immediately. Under systematic testing the harness's "All threads
// sleeping!" abort also catches the deadlock path.
//
// This test schedules many independent W → R+ [→ W] chains so that any
// individual seed of the systematic test harness has many opportunities
// to hit the race window. Each behaviour body is empty so the harness
// has minimal work between yield points.

#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

class Body
{};

// Schedule (writer, k readers, optional trailing writer) onto `c`. Each
// inner `when(c)` runs from its own outer `when()` body so they all sit
// on independent worker threads with no shared predecessor on the outer
// schedule. The order in which the inner schedules attach to `c` is
// then determined by the systematic scheduler, which gives the harness
// the freedom to interleave R1.StartEnqueue with W1.body and W1.release.
void schedule_chain(cown_ptr<Body> c, size_t readers, bool trailing_writer)
{
  // Leading writer W1.
  when() << [c]() { when(c) << [](auto) {}; };

  // k readers R1 .. Rk.
  for (size_t i = 0; i < readers; i++)
  {
    when() << [c]() { when(read(c)) << [](auto) {}; };
  }

  // Optional trailing writer W2 (exercises the writer_at_end path that
  // the walker must navigate via NextReader links rather than wrongly
  // claiming R1 as the chain terminus).
  if (trailing_writer)
  {
    when() << [c]() { when(c) << [](auto) {}; };
  }
}

void test_body(size_t chains, size_t readers, bool trailing_writer)
{
  Logging::cout() << "test_body chains=" << chains << " readers=" << readers
                  << " trailing_writer=" << trailing_writer << Logging::endl;

  // Independent cowns so each chain races on its own slot link, but they
  // all compete for the same finite set of worker threads, increasing the
  // chance of interleaving R1.StartEnqueue with W1.body completion.
  for (size_t i = 0; i < chains; i++)
  {
    auto c = make_cown<Body>();
    schedule_chain(c, readers, trailing_writer);
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  auto chains = harness.opt.is<size_t>("--chains", 4);
  auto readers = harness.opt.is<size_t>("--readers", 3);
  auto trailing_writer = !harness.opt.has("--no_trailing_writer");

  harness.run(test_body, chains, readers, trailing_writer);

  return 0;
}
