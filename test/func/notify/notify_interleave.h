// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
namespace notify_interleave
{
  /**
   * Perform a series of notifications interleaved with other work, and check
   * that we get the expected number of notifications.
   *
   * There is an interested edge case in the test that allows for a notification
   * to arrive in the phase due to the way the rescheduling a notification
   * works.
   */

  struct A : public VCown<A>
  {
    size_t notify_count = 0;
    bool run = true;
  };

  // Specifies if we use an when() to schedule the next step.
  // If false, we rescheduling synchronously, and have sequential behaviour
  // as everything is on the same cown.
  // If true, we reschedule asynchronously, and have interleaved behaviour.
  bool interleaved;

  void schedule_step(size_t count, A* a, Notification* n);

  void step(size_t count, A* a, Notification* n)
  {
    Logging::cout() << "Step: " << count << Logging::endl;
    if (count == 0)
    {
      a->run = false;
      Shared::release(ThreadAlloc::get(), n);
      Shared::release(ThreadAlloc::get(), a);
      return;
    }
    for (int i = 0; i < 5; i++)
    {
      // Pause a little in systematic testing to increase interleaving.
      for (int j = 0; j < 10; j++)
        Systematic::yield();
      Logging::cout() << "Sending notification " << i << Logging::endl;
      n->notify();
    }

    schedule_lambda(a, [a, n, count]() {
      Logging::cout() << "Notifications batch: " << a->notify_count
                      << Logging::endl;
      if (a->notify_count > 6)
      {
        // There can be atmost 6 notifications in the queue at any time.
        // There are 5 notifications sent in the loop above.
        // There can be an additional notification if there is a notification
        // that has been notified while running, and then the resumption of that
        // is in at the start of the next step.
        //
        //    notify()
        //    running
        //    notify() -> scheduled
        //    start next step
        //    resceduled notify runs
        //    notify() * 5
        //
        //  Hence, there are a maximum of 6 notifications in the queue at any
        //  time.
        abort();
      }
      if (!interleaved && a->notify_count != 1)
      {
        // The non-interleaved behaviour should have maximum consolidation, so
        // there should always be only one notification as the rest will be
        // consolidated. As the notifier holds the cown that is part of the
        // notification, there is no possibility to process any notification
        // until they are all scheduled and thus consolidated.
        abort();
      }
      a->notify_count = 0;

      schedule_step(count - 1, a, n);
    });
  }

  void schedule_step(size_t count, A* a, Notification* n)
  {
    if (interleaved)
    {
      schedule_lambda(a, [a, n, count]() { step(count, a, n); });
      Logging::cout() << "Step scheduled: " << count << Logging::endl;
    }
    else
    {
      step(count, a, n);
    }
  }

  void loop(A* a)
  {
    schedule_lambda(a, [a]() {
      if (a->run)
      {
        loop(a);
      }
      else
      {
        Shared::release(ThreadAlloc::get(), a);
      }
    });
  }

  void run_test(bool interleaved)
  {
    notify_interleave::interleaved = interleaved;

    auto a = new A;
    Notification* n = make_notification(a, [a]() {
      a->notify_count++;
      Logging::cout() << "Notification received: " << a->notify_count
                      << Logging::endl;
    });

    schedule_step(10, a, n);

    // Keep 'a' busy so it can reply to notifications quickly on a different
    // thread.
    Shared::acquire(a);
    loop(a);
  }
}
