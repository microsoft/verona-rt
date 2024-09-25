// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * This example tests the notification interleaved with when.
 *
 * Effectively a notification triggers a behaviour which triggers the original
 * notification.
 *
 * It tests that the counts are equal at each step of notifications and
 * behaviours.
 */
#include <cpp/when.h>

namespace notify_empty_queue
{
  struct MyCown : VCown<MyCown>
  {
    size_t send_count = 0;
    size_t recv_count = 0;
  };

  // Use a global to allow notification to refer to itself.
  Notification* n;

  void run_test()
  {
    auto a = new MyCown;
    n = make_notification(a, [a]() {
      Logging::cout() << "Notification received!" << Logging::endl;

      if (a->recv_count != a->send_count)
      {
        std::cout << "Received " << a->recv_count << " notifications."
                  << std::endl;
        std::cout << "Sent " << a->send_count << " notifications." << std::endl;
        abort();
      }

      if (a->recv_count == 10)
      {
        check(a->send_count == 10);
        Shared::release(n);
        Shared::release(a);
        return;
      }

      a->recv_count++;
      schedule_lambda(a, [a]() {
        Logging::cout() << "Notification sending behaviour running!"
                        << Logging::endl;
        a->send_count++;
        n->notify();
        Logging::cout() << "Notification sent!" << Logging::endl;
      });
      Logging::cout() << "Notification sending behaviour scheduled!"
                      << Logging::endl;
    });

    n->notify();
  }
}
