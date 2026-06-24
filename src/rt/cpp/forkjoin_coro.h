// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Cilk-style fork/join using C++20 coroutines on the verona-rt scheduler.
//
// No thread ever blocks. At `co_await sync()`, the coroutine suspends and
// its frame (heap-allocated by the compiler) becomes the continuation.
// The last completing child resumes it via the scheduler.
//
// Usage:
//
//   Task fib(int n, int& out) {
//     if (n < 2) { out = n; co_return; }
//     int a, b;
//     co_await spawn(fib(n-1, a));
//     co_await spawn(fib(n-2, b));
//     co_await sync();
//     out = a + b;
//   }
//
//   // Blocking entry point (from a scheduler thread):
//   int result;
//   run_sync(fib(10, result));
//   // result == 55
//
// Requires: C++20 (-std=c++20 or -fcoroutines)

#include "../sched/schedulerthread.h"
#include "../sched/work.h"

#include <atomic>
#include <coroutine>
#include <thread>
#include <utility>

namespace verona::rt::fj
{
  using Scheduler = ThreadPool<SchedulerThread>;

  // ---- Forward declarations ----
  struct Task;
  struct SyncPoint;

  // ---- SyncPoint ----
  // Shared sync state owned by the parent Task's promise.
  // Tracks outstanding children and holds the parent's coroutine handle
  // for resumption.

  struct SyncPoint
  {
    std::atomic<size_t> outstanding{0};
    std::atomic<void*> waiter{nullptr}; // coroutine_handle<>::address()

    void add_child()
    {
      outstanding.fetch_add(1, std::memory_order_relaxed);
    }

    void child_complete()
    {
      if (outstanding.fetch_sub(1, std::memory_order_seq_cst) == 1)
      {
        // Last child — try to pick up the waiter.
        void* w = waiter.exchange(nullptr, std::memory_order_seq_cst);
        if (w)
        {
          auto h = std::coroutine_handle<>::from_address(w);
          Work* work = Closure::make([h](Work*) mutable -> bool {
            h.resume();
            return true;
          });
          Scheduler::schedule(work, false);
        }
      }
    }
  };

  // ---- Task ----
  // Coroutine return type. Tasks communicate results via references
  // (like Cilk — spawn passes pointers to output locations).

  struct Task
  {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type
    {
      SyncPoint sync_point;
      SyncPoint* parent_sync{nullptr}; // parent's sync to decrement on done

      Task get_return_object()
      {
        return Task{handle_type::from_promise(*this)};
      }

      // Start suspended — caller decides when to resume (schedule).
      std::suspend_always initial_suspend() noexcept { return {}; }

      // At final suspend, notify parent (if any) and self-destroy.
      // All tasks self-manage their lifetime once scheduled.
      auto final_suspend() noexcept
      {
        struct FinalAwaiter
        {
          bool await_ready() noexcept { return false; }
          void await_suspend(handle_type h) noexcept
          {
            auto* ps = h.promise().parent_sync;
            if (ps)
              ps->child_complete();
            // Always self-destroy. Root tasks call signal_done()
            // before co_return to release the scheduler.
            h.destroy();
          }
          void await_resume() noexcept {}
        };
        return FinalAwaiter{};
      }

      void return_void() {}
      void unhandled_exception() { std::terminate(); }
    };

    handle_type handle;

    Task(handle_type h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task& operator=(Task&& o) noexcept
    {
      if (handle)
        handle.destroy();
      handle = std::exchange(o.handle, nullptr);
      return *this;
    }
    ~Task()
    {
      if (handle)
        handle.destroy();
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool done() const { return handle.done(); }
  };

  // ---- SpawnHelper ----
  // `co_await spawn(child_task)` from a parent Task:
  //   - Links child to parent's SyncPoint
  //   - Schedules child on the WSQ
  //   - Immediately resumes parent (no suspension)

  struct SpawnHelper
  {
    Task child;

    // Don't suspend the parent — spawn is non-blocking.
    // We use await_suspend returning false to mean "resume immediately".
    bool await_ready() noexcept { return false; }

    bool await_suspend(Task::handle_type parent_h) noexcept
    {
      // Link child to parent's sync point
      auto& parent_promise = parent_h.promise();
      child.handle.promise().parent_sync = &parent_promise.sync_point;
      parent_promise.sync_point.add_child();

      // Schedule child on WSQ
      auto child_h = child.handle;
      // Release ownership — child self-destroys at final_suspend.
      child.handle = nullptr;

      Work* w = Closure::make([child_h](Work*) mutable -> bool {
        child_h.resume();
        return true;
      });
      Scheduler::schedule(w, false);

      // Return false = don't actually suspend, resume parent immediately.
      return false;
    }

    void await_resume() noexcept {}
  };

  inline SpawnHelper spawn(Task child)
  {
    return SpawnHelper{std::move(child)};
  }

  // ---- SyncAwaiter ----
  // `co_await sync()` suspends the parent until all spawned children complete.
  // If no children are outstanding, doesn't suspend.

  struct SyncAwaiter
  {
    bool await_ready() noexcept { return false; }

    bool await_suspend(Task::handle_type h) noexcept
    {
      auto& sp = h.promise().sync_point;

      // Fast path: no outstanding children.
      if (sp.outstanding.load(std::memory_order_seq_cst) == 0)
        return false;

      // Publish our handle. Use seq_cst to ensure child_complete
      // observes it if it sees outstanding == 0 after our store.
      sp.waiter.store(h.address(), std::memory_order_seq_cst);

      // Double-check: children may have all completed before we stored.
      if (sp.outstanding.load(std::memory_order_seq_cst) == 0)
      {
        // Try to reclaim the waiter — if child_complete already took it
        // (exchanged to nullptr), it will resume us, so we must suspend.
        void* w =
          sp.waiter.exchange(nullptr, std::memory_order_seq_cst);
        if (w != nullptr)
        {
          // We got it back — no child took it. Don't suspend.
          return false;
        }
        // Child already took it and will schedule our resumption.
        return true;
      }
      // Children still outstanding — suspend and wait.
      return true;
    }

    void await_resume() noexcept {}
  };

  inline SyncAwaiter sync()
  {
    return SyncAwaiter{};
  }

  // ---- run_task ----
  // Entry point: schedules a root Task on the WSQ and keeps the
  // scheduler alive until the root calls signal_done().
  //
  // Takes ownership of the Task and detaches it — the coroutine
  // self-destroys at final_suspend. Call from within a scheduler
  // thread (e.g. inside a `when() << lambda`).

  inline void run_task(Task& task)
  {
    Scheduler::add_external_event_source();

    auto root_h = task.handle;
    // Detach: coroutine will self-destroy at final_suspend.
    task.handle = nullptr;

    Work* w = Closure::make([root_h](Work*) mutable -> bool {
      root_h.resume();
      return true;
    });
    Scheduler::schedule(w, false);
  }

  // Call from within the root task's final_suspend (or after sync)
  // to release the scheduler. Typically called just before co_return
  // in a root task wrapper.
  inline void signal_done()
  {
    Scheduler::remove_external_event_source();
  }

} // namespace verona::rt::fj

