// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "behaviour.h"
#include "cown.h"
#include "cown_array.h"

#include <functional>
#include <tuple>
#include <utility>
#include <verona.h>

namespace verona::cpp
{
  using namespace verona::rt;

  template<typename T>
  class acquired_cown_span
  {
    friend class When;

    Slot* array;
    size_t length_;

    acquired_cown_span(Slot* arr, size_t len) : array(arr), length_(len) {}

  public:
    acquired_cown<T> operator[](size_t i)
    {
      return acquired_cown<T>(&array[i]);
    }

    size_t length() const
    {
      return length_;
    }

    // Remove move and copy constructors to prevent copying.
    acquired_cown_span(const acquired_cown_span&) = delete;
    acquired_cown_span& operator=(const acquired_cown_span&) = delete;
    acquired_cown_span(acquired_cown_span&&) = delete;
    acquired_cown_span& operator=(acquired_cown_span&&) = delete;
  };

  template<size_t Size = 1>
  class Batch
  {
    // Verona RT collection of behaviours to be scheduled.
    std::array<BehaviourCore*, Size> when_batch;

    /// This is used to prevent the destructor from scheduling the behaviour
    /// more than once.
    /// If this batch is combined with another batch, then the destructor of
    /// the uncombined batches should not run.
    bool part_of_larger_batch{false};

    template<size_t Size_>
    friend class Batch;

    // To be initialised by the + operator.
    template<size_t Size1, size_t Size2>
    Batch(Batch<Size1>&& b1, Batch<Size2>&& b2)
    {
      b1.part_of_larger_batch = true;
      b2.part_of_larger_batch = true;
      for (size_t i = 0; i < Size1; i++)
        when_batch[i] = b1.when_batch[i];
      for (size_t i = 0; i < Size2; i++)
        when_batch[i + Size1] = b2.when_batch[i];
    }

  public:
    Batch(BehaviourCore* b) : when_batch({b})
    {
      Logging::cout() << "Batch created " << this << " contents " << b
                      << Logging::endl;
      if ((uintptr_t)b < 4096)
        abort();
    }

    Batch(const Batch&) = delete;

    ~Batch()
    {
      if (part_of_larger_batch)
        return;

      BehaviourCore::schedule_many(when_batch.data(), Size);
    }

    template<size_t Size2>
    auto operator+(Batch<Size2>&& wb) &&
    {
      return Batch<Size + Size2>(std::move(*this), std::move(wb));
    }
  };

  class When
  {
    template<typename... Args>
    friend Batch<1> when(Args&&... args);

    template<typename... Args>
    struct Last;

    template<typename T, typename... Args>
    struct Last<T, Args...>
    {
      using type = typename Last<Args...>::type;
    };

    template<typename T>
    struct Last<T>
    {
      using type = T;
    };

    template<typename Arg, typename... Args>
    static auto last(Arg&& arg, Args&&... args)
    {
      if constexpr (sizeof...(Args) == 0)
        return std::forward<Arg>(arg);
      else
        return last<Args...>(std::forward<Args>(args)...);
    }

    ///@{
    /**
     * Helper to get the type of the cown from a cown_ptr or cown_array.
     *
     * This is used to get the type of the cown in a `when` clause.
     */
    template<typename T>
    struct CownType;

    template<typename T, bool owning>
    struct CownType<cown_array<T, owning>>
    {
      using type = T;
    };

    template<typename T>
    struct CownType<cown_ptr<T>>
    {
      using type = T;
    };

    template<typename T>
    using GetCownType =
      typename CownType<std::remove_const_t<std::remove_reference_t<T>>>::type;
    ///@}

    ///@{
    /**
     * Helper to distinguish cown_ptr and cown_array.
     */
    template<typename T>
    struct IsCownArray_t
    {
      static constexpr bool value = false;
    };

    template<typename T, bool owning>
    struct IsCownArray_t<cown_array<T, owning>>
    {
      static constexpr bool value = true;
    };

    template<typename T>
    static constexpr bool IsCownArray =
      IsCownArray_t<std::remove_const_t<std::remove_reference_t<T>>>::value;
    ///@}

    struct Spec
    {
      size_t slot_count;
      size_t span_count;

      Spec operator+(Spec other) const
      {
        return {slot_count + other.slot_count, span_count + other.span_count};
      }
    };

    template<typename Arg, typename... Args>
    static Spec calculate_spec(Arg&& arg, Args&&... args)
    {
      if constexpr (sizeof...(args) == 0)
      {
        return {0, 0};
      }
      else
      {
        auto rest = calculate_spec(std::forward<Args>(args)...);
        if constexpr (IsCownArray<Arg>)
        {
          Spec s = {arg.length, 1};
          return rest + s;
        }
        else
        {
          Spec s = {1, 0};
          return rest + s;
        }
      }
    }

    // Internal structure for representing a cown acquired by a `when`.
    // This representation does not have the restrictions on lifetime
    // theat `acquired_cown` has, so we can move it around.
    template<typename T>
    struct AccessCown
    {
      Slot* slot;

      AccessCown(Slot* s) : slot(s) {}
    };

    // Internal structure for representing a cown array acquired by a `when`.
    // See AccessCown for details.
    template<typename T>
    struct AccessCownArray
    {
      Slot* slots;
      size_t length;
    };

    // build a tuple using the type signature of the cown arguments.
    // Needs to be passed the slots and lengths of any cown arrays.
    template<typename Arg, typename... Args>
    static auto construct_access_tuple(Slot* slots, size_t* lengths)
    {
      if constexpr (sizeof...(Args) == 0)
      {
        return std::make_tuple();
      }
      else
      {
        if constexpr (IsCownArray<Arg>)
        {
          size_t length = lengths[0];
          auto cown_tuple =
            std::make_tuple<AccessCownArray<GetCownType<Arg>>>({slots, length});
          return std::tuple_cat(
            cown_tuple,
            construct_access_tuple<Args...>(slots + length, lengths + 1));
        }
        else
        {
          auto cown_tuple =
            std::make_tuple<AccessCown<GetCownType<Arg>>>(slots);
          return std::tuple_cat(
            cown_tuple, construct_access_tuple<Args...>(slots + 1, lengths));
        }
      }
    }

    ///@{
    /**
     * Convert the internal representation into an unmovable/copyable one.
     */
    template<typename T>
    static auto convert_to_acquired(T);

    template<typename T>
    static auto convert_to_acquired(AccessCown<T> ac)
    {
      return acquired_cown<T>{ac.slot};
    }

    template<typename T>
    static auto convert_to_acquired(AccessCownArray<T> ac)
    {
      return acquired_cown_span<T>{ac.slots, ac.length};
    }
    ///@}

    // build a tuple using the type signature of the cown arguments.
    // Needs to be passed the slots and lengths of any cown arrays.
    template<typename Arg, typename... Args>
    static void
    initialise_slots(Slot* slots, size_t* lengths, Arg&& cp, Args&&... args)
    {
      if constexpr (sizeof...(Args) > 0)
      {
        if constexpr (IsCownArray<Arg>)
        {
          size_t length = cp.length;
          lengths[0] = length;

          for (size_t i = 0; i < length; i++)
          {
            new (&slots[i]) Slot(cp.array[i].underlying_cown());
            if constexpr (std::is_const<GetCownType<Arg>>())
            {
              slots[i].set_read_only();
            }
            if constexpr (std::is_rvalue_reference<Arg>::value)
            {
              slots[i].set_move();
            }
          }
          if constexpr (std::is_rvalue_reference<Arg>::value)
          {
            cp.steal();
          }
          initialise_slots(
            slots + length, lengths + 1, std::forward<Args>(args)...);
          return;
        }
        else
        {
          new (slots) Slot(cp.underlying_cown());
          if constexpr (std::is_const<GetCownType<Arg>>())
          {
            slots[0].set_read_only();
          }
          if constexpr (std::is_rvalue_reference<Arg>::value)
          {
            slots[0].set_move();
            cp.allocated_cown = nullptr;
          }
          initialise_slots(slots + 1, lengths, std::forward<Args>(args)...);
        }
      }
    }

    template<typename... _Args>
    static void invoke(Work* work)
    {
      using Be = typename Last<_Args...>::type;
      // Dispatch to the body of the behaviour.
      BehaviourCore* b = BehaviourCore::from_work(work);
      Be* body = b->template get_body<Be>();
      // After the body we store the lengths of any of the spans.
      size_t* lengths = (size_t*)(body + 1);

      auto* slots = b->get_slots();

      // Get the cown array from the slots.
      auto cown_tuple = construct_access_tuple<_Args...>(slots, lengths);

      std::apply(
        [&](auto&&... args) { (*body)(convert_to_acquired(args)...); },
        cown_tuple);

      if (rerun())
      {
        rerun() = false;
        Scheduler::schedule(work);
        return;
      }

      // Dealloc behaviour
      body->~Be();

      BehaviourCore::finished(work);
    }

  public:
    static bool& rerun()
    {
      static thread_local bool rerun = false;
      return rerun;
    }
  };

  template<typename... Args>
  Batch<1> when(Args&&... args)
  {
    // Calculate the number of slots and spans required for the behaviour.
    auto spec = When::calculate_spec(std::forward<Args>(args)...);

    // Extract behaviour type
    using Be = typename When::Last<Args...>::type;

    // These assertions are basically checking that we won't break any
    // alignment assumptions on Be.  If we add some actual alignment, then
    // this can be improved.
    static_assert(
      alignof(Be) <= sizeof(void*), "Alignment not supported, yet!");

    // The payload is the Be followed by the size of each of the spans.
    auto behaviour_core = BehaviourCore::make(
      spec.slot_count,
      When::invoke<Args...>,
      sizeof(Be) + spec.span_count * sizeof(size_t));

    // After the body we store the lengths of any of the spans.
    auto body = behaviour_core->template get_body<Be>();
    size_t* lengths = (size_t*)(body + 1);
    // Fill in slots and span sizes
    When::initialise_slots(
      behaviour_core->get_slots(), lengths, std::forward<Args>(args)...);

    new (body) Be(std::forward<Be>(When::last(std::forward<Args>(args)...)));

    return {behaviour_core};
  }
} // namespace verona::cpp
