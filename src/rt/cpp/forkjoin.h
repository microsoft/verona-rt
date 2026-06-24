// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Cilk-style spawn/sync on top of the verona-rt work-stealing scheduler.
//
// This provides structured fork/join parallelism using the same WSQ and
// scheduler infrastructure as BoC. It sits at the same architectural
// level as the BoC layer — an alternative producer of Work* items.
//
// Key design choices:
// - Closure-based (not stackful): each sync point becomes a continuation
//   closure that is scheduled when the last child completes.
// - Child-steal: spawned tasks go into the WSQ and can be stolen.
// - Non-blocking sync: the parent task suspends; its continuation is
//   re-enqueued by the last completing child.
//
// Usage:
//
//   // Inside a scheduler thread (from a when-block, run(), etc.):
//
//   // Simple fork/join with results:
//   std::atomic<int> result{0};
//   fork_join(2, [&](size_t i) {
//     // Body runs once per child (i = 0, 1)
//     result += compute(i);
//   }, [&] {
//     // Continuation: runs after ALL children complete
//     printf("total = %d\n", result.load());
//   });
//
//   // Or the explicit scope API for variable spawn counts:
//   auto* frame = TaskFrame::create([&] {
//     // continuation after sync
//   });
//   frame->spawn([&] { /* child 1 */ });
//   frame->spawn([&] { /* child 2 */ });
//   frame->arm(); // arms the countdown; continuation fires when all done
//
// IMPORTANT: After arm(), the calling task must return to the scheduler.
// Do not access stack-local state after arm() — the continuation may
// have already fired on another thread.

#include "../sched/schedulerthread.h"
#include "../sched/work.h"

#include <atomic>
#include <thread>
#include <type_traits>

namespace verona::rt::forkjoin
{

  // ---- TaskFrame ----
  //
  // Tracks outstanding children. When the countdown reaches zero after
  // arm() is called, the continuation is scheduled.
  //
  // Lifecycle:
  //   1. create(continuation) → frame with outstanding=0, armed=false
  //   2. frame->spawn(f) increments outstanding, enqueues child
  //   3. frame->arm() sets armed=true; if outstanding==0, fires immediately
  //   4. Each child_complete() decrements; last one (if armed) fires cont.
  //   5. Continuation runs → frame is deallocated.

  struct TaskFrame
  {
    std::atomic<size_t> outstanding{0};
    std::atomic<bool> armed{false};
    Work* continuation{nullptr};

    void child_complete()
    {
      if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
      {
        // Last child. Fire if armed.
        if (armed.load(std::memory_order_acquire))
          fire();
      }
    }

    void arm()
    {
      armed.store(true, std::memory_order_release);
      // If all children already finished before arm(), fire now.
      if (outstanding.load(std::memory_order_acquire) == 0)
        fire();
    }

    void fire()
    {
      Scheduler::schedule(continuation, false);
    }

    // Spawn a child task bound to this frame.
    template<typename F>
    void spawn(F&& task)
    {
      outstanding.fetch_add(1, std::memory_order_relaxed);

      Work* w = Closure::make(
        [this, task = std::forward<F>(task)](Work*) mutable -> bool {
          task();
          child_complete();
          return true;
        });

      Scheduler::schedule(w, false);
    }

    // Factory: create a frame with a continuation closure.
    // The continuation is responsible for deallocating the frame.
    template<typename F>
    static TaskFrame* create(F&& cont)
    {
      // Allocate frame
      void* mem = heap::alloc<sizeof(TaskFrame)>();
      auto* frame = new (mem) TaskFrame();

      // Build continuation that runs cont then frees frame
      frame->continuation = Closure::make(
        [frame, cont = std::forward<F>(cont)](Work*) mutable -> bool {
          cont();
          heap::dealloc(frame);
          return true;
        });

      return frame;
    }
  };

  // ---- fork_join_sync: simple blocking fork/join for leaf parallelism ----
  //
  // Spawns children onto the scheduler, then yields to OS until all
  // children complete. Children run on other scheduler threads.
  //
  // LIMITATIONS:
  // - Requires at least 2 scheduler threads (caller is blocked waiting).
  // - Must NOT be nested: nested blocking sync will deadlock when blocked
  //   threads exceed available scheduler threads.
  // - For nested parallelism, use the continuation-based TaskFrame API.

  class BlockingScope
  {
    std::atomic<size_t> remaining{0};

  public:
    template<typename F>
    void spawn(F&& task)
    {
      remaining.fetch_add(1, std::memory_order_relaxed);

      Work* w = Closure::make(
        [this, task = std::forward<F>(task)](Work*) mutable -> bool {
          task();
          remaining.fetch_sub(1, std::memory_order_release);
          return true;
        });

      Scheduler::schedule(w, false);
    }

    bool done() const
    {
      return remaining.load(std::memory_order_acquire) == 0;
    }
  };

  template<typename F>
  void fork_join_sync(F&& body)
  {
    BlockingScope scope;
    body(scope);

    // Brief spin before OS yield — allows fast completion without
    // context switch overhead.
    for (size_t spin = 0; !scope.done(); spin++)
    {
      if (spin < 1000)
      {
#if defined(__aarch64__)
        asm volatile("yield");
#elif defined(__x86_64__)
        asm volatile("pause");
#endif
      }
      else
      {
        std::this_thread::yield();
        spin = 0;
      }
    }
  }

  // ---- Convenience: parallel_for ----

  template<typename F>
  void parallel_for(size_t n, size_t chunk_size, F&& body)
  {
    if (n == 0)
      return;
    if (chunk_size == 0)
      chunk_size = 1;

    size_t num_chunks = (n + chunk_size - 1) / chunk_size;

    BlockingScope scope;
    for (size_t c = 0; c < num_chunks - 1; c++)
    {
      size_t start = c * chunk_size;
      size_t end = start + chunk_size;
      scope.spawn([&body, start, end] {
        for (size_t i = start; i < end; i++)
          body(i);
      });
    }

    // Inline the last chunk
    size_t last_start = (num_chunks - 1) * chunk_size;
    for (size_t i = last_start; i < n; i++)
      body(i);

    for (size_t spin = 0; !scope.done(); spin++)
    {
      if (spin < 1000)
      {
#if defined(__aarch64__)
        asm volatile("yield");
#elif defined(__x86_64__)
        asm volatile("pause");
#endif
      }
      else
      {
        std::this_thread::yield();
        spin = 0;
      }
    }
  }

} // namespace verona::rt::forkjoin

