// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "behaviour.h"
#include "cown.h"
#include "cown_array.h"

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <verona.h>

namespace verona::cpp
{
  using namespace verona::rt;

  template<typename T>
  class acquired_cown_span
  {
    template<typename... Args>
    friend class PreWhen;

    friend class When;

    Slot* array;
    size_t length_;

    acquired_cown_span(Slot* arr, size_t len) : array(arr), length_(len) {}

  public:
    acquired_cown<T> operator[](size_t i)
    {
      assert(i < length_);
      return acquired_cown<T>(&array[i]);
    }

    size_t length() const
    {
      return length_;
    }

    acquired_cown_span(const acquired_cown_span&) = delete;
    acquired_cown_span& operator=(const acquired_cown_span&) = delete;
    acquired_cown_span(acquired_cown_span&&) = delete;
    acquired_cown_span& operator=(acquired_cown_span&&) = delete;
  };

  /**
   * Batch accumulates BehaviourCore pointers and schedules them atomically
   * on destruction. Batches are non-copyable and non-movable; they exist only
   * as temporaries within a single expression. The `+` operator (&&-qualified)
   * combines temporaries into a larger batch via C++17 guaranteed elision.
   *
   * LIFETIME REQUIREMENT: If any `when()` in the batch borrowed a cown_ptr
   * (lvalue path), that cown_ptr must remain alive until this Batch destructs.
   * In the normal single-expression pattern
   *   `(when(c1) << f1) + (when(c2) << f2);`
   * this holds naturally because all sources are alive for the full statement.
   */
  template<size_t Size = 1>
  class Batch
  {
    std::array<BehaviourCore*, Size> when_batch;
    bool part_of_larger_batch{false};

    template<size_t Size_>
    friend class Batch;

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
    Batch(BehaviourCore* b) : when_batch({b}) {}

    Batch(const Batch&) = delete;
    Batch(Batch&&) = delete;
    Batch& operator=(const Batch&) = delete;
    Batch& operator=(Batch&&) = delete;

    ~Batch()
    {
      if (part_of_larger_batch)
        return;

      // Filter out nulls (from zero-cown when() calls).
      BehaviourCore* valid[Size];
      size_t valid_count = 0;
      for (size_t i = 0; i < Size; i++)
        if (when_batch[i] != nullptr)
          valid[valid_count++] = when_batch[i];

      if (valid_count > 0)
        BehaviourCore::schedule(valid, valid_count);
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
    friend class PreWhen;

    ///@{
    /**
     * Helper to get the type of the cown from a cown_ptr or cown_array.
     */
    template<typename T>
    struct CownType;

    template<typename T>
    struct CownType<cown_array<T>>
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

    template<typename T>
    struct IsCownArray_t<cown_array<T>>
    {
      static constexpr bool value = true;
    };

    template<typename T>
    static constexpr bool IsCownArray =
      IsCownArray_t<std::remove_const_t<std::remove_reference_t<T>>>::value;
    ///@}

    ///@{
    /**
     * Storage helpers for PreWhen.
     *
     * Lvalue cown args are stored as const pointers (borrowed — no refcount
     * increment).  Rvalue cown args are stored by value (owned — the
     * PreWhen takes the caller's reference via move).
     *
     * ArgStorage<Arg> selects the type.  make_storage / deref provide
     * construction and uniform access.
     */
    template<typename Arg>
    using ArgStorage = std::conditional_t<
      std::is_lvalue_reference_v<Arg>,
      std::remove_reference_t<Arg> const*,
      std::decay_t<Arg>>;

    template<typename Arg>
    static ArgStorage<Arg> make_storage(Arg&& a)
    {
      if constexpr (std::is_lvalue_reference_v<Arg>)
        return &a;
      else
        return std::move(a);
    }

    /// Dereference a stored element to get a uniform reference.
    template<typename S>
    static auto& deref(S& s)
    {
      if constexpr (std::is_pointer_v<std::decay_t<S>>)
        return *s;
      else
        return s;
    }

    /// True when the stored element is a borrowed pointer (lvalue origin).
    template<typename S>
    static constexpr bool is_borrowed_v = std::is_pointer_v<std::decay_t<S>>;

    /// Get the raw cown/array type from a stored element type, stripping
    /// the pointer wrapper and const.
    template<typename S>
    using RawStored = std::remove_const_t<std::conditional_t<
      std::is_pointer_v<std::decay_t<S>>,
      std::remove_pointer_t<std::decay_t<S>>,
      std::decay_t<S>>>;
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

    template<typename Stored, typename... Rest>
    static Spec calculate_spec(const Stored& s, const Rest&... rest)
    {
      Spec here;
      if constexpr (IsCownArray<RawStored<Stored>>)
        here = {deref(s).length, 1};
      else
        here = {1, 0};

      if constexpr (sizeof...(Rest) > 0)
        return here + calculate_spec(rest...);
      else
        return here;
    }

    // Internal structure for representing a cown acquired by a `when`.
    template<typename T>
    struct AccessCown
    {
      Slot* slot;
      AccessCown(Slot* s) : slot(s) {}
    };

    // Internal structure for representing a cown array acquired by a `when`.
    template<typename T>
    struct AccessCownArray
    {
      Slot* slots;
      size_t length;
    };

    // Build a tuple using the type signature of the cown arguments.
    template<typename Arg, typename... Args>
    static auto construct_access_tuple(Slot* slots, size_t* lengths)
    {
      if constexpr (sizeof...(Args) == 0)
      {
        // Last type arg — this is the terminal; return empty tuple.
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

    /// Initialise slots from stored cown arguments.
    ///
    /// Owned args (rvalues): set_move + null allocated_cown to transfer the
    /// reference to the slot.  The runtime's acquire_with_transfer sees
    /// transfer=1 and avoids a redundant acquire.
    ///
    /// Borrowed args (lvalues): no set_move, no null.  The original variable
    /// keeps its reference.  The runtime acquires independently if needed
    /// (head of queue).  Zero refcount overhead for the borrow.
    template<typename Stored, typename... Rest>
    static void
    initialise_slots(Slot* slots, size_t* lengths, Stored& s, Rest&... rest)
    {
      auto& cp = deref(s);
      constexpr bool borrowed = is_borrowed_v<Stored>;

      if constexpr (IsCownArray<RawStored<Stored>>)
      {
        size_t length = cp.length;
        lengths[0] = length;

        for (size_t i = 0; i < length; i++)
        {
          new (&slots[i]) Slot(cp.array[i].underlying_cown());
          if constexpr (std::is_const<GetCownType<RawStored<Stored>>>())
          {
            slots[i].set_read_only();
          }
          if constexpr (!borrowed)
          {
            slots[i].set_move();
            cp.array[i].allocated_cown = nullptr;
          }
        }
        if constexpr (sizeof...(Rest) > 0)
          initialise_slots(slots + length, lengths + 1, rest...);
      }
      else
      {
        new (slots) Slot(cp.underlying_cown());
        if constexpr (std::is_const<GetCownType<RawStored<Stored>>>())
        {
          slots[0].set_read_only();
        }
        if constexpr (!borrowed)
        {
          slots[0].set_move();
          cp.allocated_cown = nullptr;
        }
        if constexpr (sizeof...(Rest) > 0)
          initialise_slots(slots + 1, lengths, rest...);
      }
    }

    /// Invoke function stored in BehaviourCore. Reconstructs acquired_cowns
    /// from slots at runtime.
    template<typename Be, typename... CownArgs>
    static void invoke(Work* work)
    {
      BehaviourCore* b = BehaviourCore::from_work(work);
      Be* body = b->template get_body<Be>();
      constexpr size_t lengths_offset =
        (sizeof(Be) + alignof(size_t) - 1) & ~(alignof(size_t) - 1);
      size_t* lengths = reinterpret_cast<size_t*>(
        reinterpret_cast<char*>(body) + lengths_offset);

      auto* slots = b->get_slots();

      // Reconstruct access tuple from slots and lengths.
      // We append a dummy void* type to CownArgs to match
      // construct_access_tuple's terminal condition (sizeof...(Args)==0).
      auto cown_tuple =
        construct_access_tuple<CownArgs..., void*>(slots, lengths);

      std::apply(
        [&](auto&&... args) { (*body)(convert_to_acquired(args)...); },
        cown_tuple);

      if (Behaviour::behaviour_rerun())
      {
        Behaviour::behaviour_rerun() = false;
        Scheduler::schedule(work);
        return;
      }

      body->~Be();
      BehaviourCore::finished(work);
    }
  };

  /**
   * Staging object for `when(cowns...) << lambda`.
   *
   * Lvalue cown args are borrowed (stored as pointers — no refcount).
   * Rvalue cown args are owned (stored by value — ref transferred to slot
   * via set_move + null).
   */
  template<typename... Args>
  class PreWhen
  {
    std::tuple<When::ArgStorage<Args>...> cown_args;

    template<typename... As>
    friend auto when(As&&... args);

    PreWhen(Args&&... args)
    : cown_args(When::make_storage<Args>(std::forward<Args>(args))...)
    {}

    PreWhen(const PreWhen&) = delete;
    PreWhen(PreWhen&&) = delete;
    PreWhen& operator=(const PreWhen&) = delete;
    PreWhen& operator=(PreWhen&&) = delete;

  public:
    template<typename F>
    auto operator<<(F&& f) &&
    {
      Scheduler::stats().behaviour(sizeof...(Args));

      if constexpr (sizeof...(Args) == 0)
      {
        // No cowns — execute directly via scheduler.
        verona::rt::schedule_lambda(std::forward<F>(f));
        // Return a null batch (filtered out in Batch destructor).
        return Batch<1>(nullptr);
      }
      else
      {
        // Calculate slot and span counts from the held cown arguments.
        auto spec = std::apply(
          [](auto&... args) { return When::calculate_spec(args...); },
          cown_args);

        using Be = std::remove_reference_t<F>;

        static_assert(
          alignof(Be) <= sizeof(void*), "Alignment not supported, yet!");

        // Allocate BehaviourCore: slots + body(F) + aligned span lengths
        constexpr size_t lengths_offset =
          (sizeof(Be) + alignof(size_t) - 1) & ~(alignof(size_t) - 1);
        auto* behaviour_core = BehaviourCore::make(
          spec.slot_count,
          When::invoke<Be, Args...>,
          lengths_offset + spec.span_count * sizeof(size_t));

        auto* body = behaviour_core->template get_body<Be>();
        size_t* lengths = reinterpret_cast<size_t*>(
          reinterpret_cast<char*>(body) + lengths_offset);

        // Initialise slots directly from cown arguments.
        std::apply(
          [&](auto&... args) {
            When::initialise_slots(
              behaviour_core->get_slots(), lengths, args...);
          },
          cown_args);

        // Placement-new the user's lambda into the body.
        new (body) Be(std::forward<F>(f));

        return Batch<1>(behaviour_core);
      }
    }
  };

  /**
   * Implements a Verona-like `when` statement.
   *
   *   when(cown1, ..., cownn) << [](acquired_cown<T1>, ...) { ... };
   */
  template<typename... Args>
  auto when(Args&&... args)
  {
    return PreWhen<Args...>(std::forward<Args>(args)...);
  }

} // namespace verona::cpp
