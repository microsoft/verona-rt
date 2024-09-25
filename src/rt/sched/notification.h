// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "behaviourcore.h"
#include "shared.h"

namespace verona::rt
{
  class Notification : public Shared
  {
    /**
     * Used to wrap the behaviour closure with a pointer to the notification
     */
    template<typename Be>
    struct BehaviourWrapper
    {
      Notification* notification;
      Be body;
    };

    enum class Status
    {
      /// Specifies a notification has been requested since it last started to
      /// run. Hence, it has been scheduled or is about to be.
      Requested,
      /// Specifies a notification is currently running, and no requests have
      /// been made since it started
      Running,
      /// Specifies that the notification is not running and not requested.
      Waiting,
    };

    /// The status of the notification.
    std::atomic<Status> status{Status::Waiting};

    /// The behaviour that is used to process the notification.
    BehaviourCore* behaviour = nullptr;

    /**
     * The descriptor for the notification.
     */
    template<typename Be>
    static const Descriptor* descriptor()
    {
      static constexpr Descriptor desc = {
        vsizeof<Notification>, gc_trace, nullptr, nullptr, destruct<Be>};
      return &desc;
    }

    /**
     * The function to invoke on notification being scheduled.
     */
    template<typename Be>
    static void invoke(Work* work)
    {
      // Dispatch to the body of the behaviour.
      BehaviourCore* behaviour = BehaviourCore::from_work(work);
      BehaviourWrapper<Be>* wrapper =
        behaviour->template get_body<BehaviourWrapper<Be>>();
      Be& body = wrapper->body;
      auto notification = wrapper->notification;
      Logging::cout() << "Notification: Invoked: " << notification << std::endl;
      notification->set_running();

      (body)();

      behaviour->release_all();
      Logging::cout() << "Notification: Released all: " << notification
                      << std::endl;

      behaviour->reset();

      notification->finished_running();
    }

    /**
     * The destructor for the notification.  This is embedded in the
     * descriptor.  It must tidy up the closure if the notification can
     * never be called again.
     */
    template<typename Be>
    static void destruct(Object* self)
    {
      Logging::cout() << "Notification: Destruct: " << self << std::endl;
      auto notification = static_cast<Notification*>(self);
      assert(notification->status == Status::Waiting);

      notification->behaviour->template get_body<BehaviourWrapper<Be>>()
        ->body.~Be();

      auto* slots = notification->behaviour->get_slots();
      for (size_t i = 0; i < notification->behaviour->count; i++)
      {
        Shared::release(ThreadAlloc::get(), slots[i].cown());
      }

      // Need to dealloc using ABA protection for fields relating to work.
      notification->behaviour->as_work()->dealloc();
    }

    /**
     * TODO: This needs to do something with Be.
     */
    static void gc_trace(const Object*, ObjectStack&) {}

    void set_running()
    {
      assert(status == Status::Requested);
      Systematic::yield();
      status = Status::Running;
      Systematic::yield();
      Logging::cout() << "Notification: Set running: " << (int)status.load()
                      << std::endl;
    }

    void finished_running()
    {
      // Check status to see if notification occurred during running.
      assert(status != Status::Waiting);
      auto expected = Status::Running;
      if (status.compare_exchange_strong(expected, Status::Waiting))
      {
        Systematic::yield();
        Logging::cout() << "Notification: Finished running: "
                        << (int)status.load() << std::endl;
        Shared::release(ThreadAlloc::get(), this);
        return;
      }

      Systematic::yield();
      Logging::cout() << "Notification: Rescheduling notification: "
                      << (int)status.load() << std::endl;
      schedule();
    }

    void schedule()
    {
      assert(status == Status::Requested);
      Logging::cout() << "Notification: Scheduling: " << std::endl;
      BehaviourCore::schedule_many(&behaviour, 1);
    }

  public:
    /**
     * Request the notification is run.  This does not perform allocation, so
     * can be called from restricted contexts such as signal handlers.
     * Multiple requests are coalesced into a single notification.
     * A notify that is triggered after the notification starts running will
     * trigger it to reschedule itself on completion.
     */
    void notify()
    {
      if (status.exchange(Status::Requested) == Status::Waiting)
      {
        Systematic::yield();
        Logging::cout() << "Notification: Notifying: scheduled "
                        << (int)status.load() << std::endl;
        Shared::acquire(this);
        schedule();
      }
      else
      {
        Systematic::yield();
        Logging::cout() << "Notification: Notifying: already running"
                        << (int)status.load() << std::endl;
      }
    }

    /**
     * @brief Construct a new Notification object
     *
     * @tparam Be - The type of the closure that is run
     * @tparam Args - The types of the arguments to construct the closure
     * @param count - The number of cowns required
     * @param requests - the array of requested cowns to be used
     * @param args - The arguments to construct the closure
     * @return Notification* - a shared object that can be repeatedly notified
     * to run the closure on the requested cowns.
     */
    template<typename Be, typename... Args>
    static Notification* make(size_t count, Request* requests, Args... args)
    {
      // These assertions are basically checking that we won't break any
      // alignment assumptions on Be.  If we add some actual alignment, then
      // this can be improved.
      static_assert(
        alignof(Be) <= sizeof(void*), "Alignment not supported, yet!");

      // Allocate the behaviour object.
      auto behaviour_core =
        BehaviourCore::make(count, invoke<Be>, sizeof(BehaviourWrapper<Be>));
      auto wrapper = behaviour_core->template get_body<BehaviourWrapper<Be>>();
      new (&(wrapper->body)) Be(std::forward<Args>(args)...);

      // Allocate the notification object.
      void* base = ThreadAlloc::get().alloc<vsizeof<Notification>>();
      Object* o = Object::register_object(base, descriptor<Be>());
      auto notification = new (o) Notification();

      // Tie them together.
      notification->behaviour = behaviour_core;
      wrapper->notification = notification;

      // Set up the slots.
      auto* slots = behaviour_core->get_slots();
      for (size_t i = 0; i < count; i++)
      {
        Shared::acquire(requests[i].cown());
        new (&slots[i]) Slot(requests[i].cown());
      }

      return notification;
    }
  };
} // namespace verona::rt