// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../sched/behaviour.h"
#include "cown.h"

#include <functional>
#include <tuple>
#include <utility>
#include <verona.h>

namespace verona::cpp
{
  using namespace verona::rt;

  template<typename T>
  struct cown_ptr_span
  {
    cown_ptr<T>* array;
    size_t length;
  };

  template<typename T>
  struct actual_cown_ptr_span
  {
    ActualCown<std::remove_const_t<T>>** array;
    size_t length;

    actual_cown_ptr_span()
    {
      length = 0;
      array = nullptr;
    }

    actual_cown_ptr_span(actual_cown_ptr_span&& old)
    {
      length = old.length;
      old.length = 0;
      array = old.array;
      old.array = nullptr;
    }

    actual_cown_ptr_span& operator=(actual_cown_ptr_span&& old)
    {
      if (array)
        snmalloc::ThreadAlloc::get().dealloc(array);

      length = old.length;
      old.length = 0;
      array = old.array;
      old.array = nullptr;

      return *this;
    }
  };

  template<typename T>
  struct acquired_cown_span
  {
    acquired_cown<T>* array;
    size_t length;
  };

  /**
   * Used to track the type of access request by embedding const into
   * the type T, or not having const.
   */
  template<typename T>
  class Access
  {
    using Type = T;
    ActualCown<std::remove_const_t<T>>* t;
    bool is_move;

  public:
    Access(const cown_ptr<T>& c) : t(c.allocated_cown), is_move(false)
    {
      assert(c.allocated_cown != nullptr);
    }

    Access(cown_ptr<T>&& c) : t(c.allocated_cown)
    {
      assert(c.allocated_cown != nullptr);
      c.allocated_cown = nullptr;
    }

    template<typename F, typename... Args>
    friend class When;
  };

  template<typename T>
  class AccessBatch
  {
    using Type = T;
    actual_cown_ptr_span<T> span;
    acquired_cown<T>* acq_array;
    bool is_move;

  public:
    AccessBatch(cown_ptr_span<T> ptr_span) : is_move(false)
    {
      // Allocate the actual_cown and the acquired_cown array
      // The acquired_cown array is after the actual_cown one
      size_t actual_size =
        ptr_span.length * sizeof(ActualCown<std::remove_const_t<T>>*);
      size_t acq_size =
        ptr_span.length * sizeof(acquired_cown<std::remove_const_t<T>>);
      span.array = reinterpret_cast<ActualCown<std::remove_const_t<T>>**>(
        snmalloc::ThreadAlloc::get().alloc(actual_size + acq_size));

      for (size_t i = 0; i < ptr_span.length; i++)
      {
        span.array[i] = ptr_span.array[i].allocated_cown;
      }
      span.length = ptr_span.length;

      acq_array =
        reinterpret_cast<acquired_cown<T>*>((char*)(span.array) + actual_size);

      for (size_t i = 0; i < ptr_span.length; i++)
      {
        new (&acq_array[i]) acquired_cown<T>(*ptr_span.array[i].allocated_cown);
      }
    }

    AccessBatch(AccessBatch&& old)
    {
      span = std::move(old.span);
      acq_array = old.acq_array;
      old.acq_array = nullptr;
      is_move = old.is_move;
    }

    ~AccessBatch()
    {
      if (span.array)
      {
        snmalloc::ThreadAlloc::get().dealloc(span.array);
      }
    }

    AccessBatch& operator=(AccessBatch&&) = delete;
    AccessBatch(const AccessBatch&) = delete;
    AccessBatch& operator=(const AccessBatch&) = delete;

    template<typename F, typename... Args>
    friend class When;
  };

  template<typename T>
  auto convert_access(const cown_ptr<T>& c)
  {
    return Access<T>(c);
  }

  template<typename T>
  auto convert_access(cown_ptr<T>&& c)
  {
    return Access<T>(c);
  }

  template<typename T>
  auto convert_access(cown_ptr_span<T> c)
  {
    return AccessBatch<T>(c);
  }

  template<typename... Args>
  class Batch
  {
    /// This is a tuple of
    ///    (exists Ts. When<Ts>)
    /// As existential types are not supported this is using inferred template
    /// parameters.
    std::tuple<Args...> when_batch;

    /// This is used to prevent the destructor from scheduling the behaviour
    /// more than once.
    /// If this batch is combined with another batch, then the destructor of
    /// the uncombined batches should not run.
    bool part_of_larger_batch = false;

    template<typename... Args2>
    friend class Batch;

    template<size_t index = 0>
    void create_behaviour(BehaviourCore** barray)
    {
      if constexpr (index >= sizeof...(Args))
      {
        return;
      }
      else
      {
        auto&& w = std::get<index>(when_batch);
        // Add the behaviour here
        auto t = w.to_tuple();
        barray[index] = Behaviour::prepare_to_schedule<
          typename std::remove_reference<decltype(std::get<2>(t))>::type>(
          std::move(std::get<0>(t)),
          std::move(std::get<1>(t)),
          std::move(std::get<2>(t)));
        create_behaviour<index + 1>(barray);
      }
    }

  public:
    Batch(std::tuple<Args...> args) : when_batch(std::move(args)) {}

    Batch(const Batch&) = delete;

    ~Batch()
    {
      if constexpr (sizeof...(Args) > 0)
      {
        if (part_of_larger_batch)
          return;

        BehaviourCore* barray[sizeof...(Args)];
        create_behaviour(barray);

        BehaviourCore::schedule_many(barray, sizeof...(Args));
      }
    }

    template<typename... Args2>
    auto operator+(Batch<Args2...>&& wb)
    {
      wb.part_of_larger_batch = true;
      this->part_of_larger_batch = true;
      return Batch<Args..., Args2...>(
        std::tuple_cat(std::move(this->when_batch), std::move(wb.when_batch)));
    }
  };

  /**
   * Represents a single when statement.
   *
   * It carries all the information needed to create the behaviour.
   */
  template<typename F, typename... Args>
  class When
  {
    template<class T>
    struct is_read_only : std::false_type
    {};
    template<class T>
    struct is_read_only<Access<const T>> : std::true_type
    {};

    template<class T>
    struct is_batch : std::false_type
    {};
    template<class T>
    struct is_batch<AccessBatch<T>> : std::true_type
    {};

    template<typename... Args2>
    friend class Batch;

    /// Set of cowns used by this behaviour.
    std::tuple<Args...> cown_tuple;

    /// The closure to be executed.
    F f;

    /// Used as a temporary to build the behaviour.
    /// The stack lifetime is tricky, and this avoids
    /// a heap allocation.
    Request requests[sizeof...(Args)];

    // If cown_ptr spans provided more requests are required
    // and dynamically allocated
    Request* req_extended;
    bool is_req_extended;

    /**
     * This uses template programming to turn the std::tuple into a C style
     * stack allocated array.
     * The index template parameter is used to perform each the assignment for
     * each index.
     */
    template<size_t index = 0>
    void array_assign(Request* requests)
    {
      if constexpr (index >= sizeof...(Args))
      {
        return;
      }
      else
      {
        auto& p = std::get<index>(cown_tuple);
        if constexpr (is_batch<
                        typename std::remove_reference<decltype(p)>::type>())
        {
          for (size_t i = 0; i < p.span.length; i++)
          {
            if constexpr (is_read_only<decltype(p)>())
              *requests = Request::read(p.span.array[i]);
            else
              *requests = Request::write(p.span.array[i]);

            requests++;
          }
        }
        else
        {
          if constexpr (is_read_only<decltype(p)>())
            *requests = Request::read(p.t);
          else
            *requests = Request::write(p.t);

          if (p.is_move)
            requests->mark_move();

          assert(requests->cown() != nullptr);
          requests++;
        }
        array_assign<index + 1>(requests);
      }
    }

    template<size_t index = 0>
    size_t get_cown_count(size_t count = 0)
    {
      if constexpr (index >= sizeof...(Args))
      {
        return count;
      }
      else
      {
        auto& p = std::get<index>(cown_tuple);
        size_t to_add;
        if constexpr (is_batch<
                        typename std::remove_reference<decltype(p)>::type>())
          to_add = p.span.length;
        else
          to_add = 1;

        return get_cown_count<index + 1>(count + to_add);
      }
    }

    /**
     * Converts a single `cown_ptr` into a `acquired_cown`.
     *
     * Needs to be a separate function for the template parameter to work.
     */
    template<typename C>
    static auto access_to_acquired(AccessBatch<C>& c)
    {
      return acquired_cown_span<C>{c.acq_array, c.span.length};
    }

    template<typename C>
    static auto access_to_acquired(Access<C>& c)
    {
      assert(c.t != nullptr);
      return acquired_cown<C>(*c.t);
    }

    auto to_tuple()
    {
      if constexpr (sizeof...(Args) == 0)
      {
        return std::make_tuple(std::forward<F>(f));
      }
      else
      {
        Request* r;
        if (is_req_extended)
          r = req_extended;
        else
          r = reinterpret_cast<Request*>(&requests);

        array_assign(r);

        return std::make_tuple(
          sizeof...(Args),
          r,
          [f = std::move(f), cown_tuple = std::move(cown_tuple)]() mutable {
            /// Effectively converts ActualCown<T>... to
            /// acquired_cown... .
            auto lift_f = [f = std::move(f)](Args... args) mutable {
              std::move(f)(access_to_acquired<typename Args::Type>(args)...);
            };

            std::apply(std::move(lift_f), std::move(cown_tuple));
          });
      }
    }

  public:
    When(F&& f_) : f(std::forward<F>(f_)) {}

    When(F&& f_, std::tuple<Args...> cown_tuple_)
    : f(std::forward<F>(f_)),
      cown_tuple(std::move(cown_tuple_)),
      is_req_extended(false)
    {
      const size_t req_count = get_cown_count();
      if (req_count > sizeof...(Args))
      {
        is_req_extended = true;
        req_extended = reinterpret_cast<Request*>(
          snmalloc::ThreadAlloc::get().alloc(req_count * (sizeof(Request))));
      }
    }

    When(When&& o)
    : cown_tuple(std::move(o.cown_tuple)),
      f(std::forward<F>(o.f)),
      is_req_extended(o.is_req_extended),
      req_extended(o.req_extended)
    {
      o.req_extended = nullptr;
      o.is_req_extended = false;
    }

    When(const When&) = delete;

    ~When()
    {
      if (is_req_extended)
      {
        snmalloc::ThreadAlloc::get().dealloc(req_extended);
      }
    }
  };

  /**
   * Class for staging the when creation.
   *
   * Do not call directly use `when`
   *
   * This provides an operator << to apply the closure.  This allows the
   * argument order to be more sensible, as variadic arguments have to be last.
   *
   *   when (cown1, ..., cownn) << closure;
   *
   * Allows the variadic number of cowns to occur before the closure.
   */
  template<typename... Args>
  class PreWhen
  {
    // Note only requires friend when Args2 == Args
    // but C++ doesn't like this.
    template<typename... Args2>
    friend auto when(Args2&&... args);

    /**
     * Internally uses AcquiredCown.  The cown is only acquired after the
     * behaviour is scheduled.
     */
    std::tuple<Args...> cown_tuple;

    PreWhen(Args... args) : cown_tuple(std::move(args)...) {}

  public:
    template<typename F>
    auto operator<<(F&& f)
    {
      Scheduler::stats().behaviour(sizeof...(Args));

      if constexpr (sizeof...(Args) == 0)
      {
        // Execute now atomic batch makes no sense.
        verona::rt::schedule_lambda(std::forward<F>(f));
        return Batch(std::make_tuple());
      }
      else
      {
        return Batch(
          std::make_tuple(When(std::forward<F>(f), std::move(cown_tuple))));
      }
    }
  };

  /**
   * Template deduction guide for Access.
   */
  template<typename T>
  Access(const cown_ptr<T>&) -> Access<T>;

  /**
   * Template deduction guide for Batch.
   */
  template<typename... Args>
  Batch(std::tuple<Args...>) -> Batch<Args...>;

  /**
   * Implements a Verona-like `when` statement.
   *
   * Uses `<<` to apply the closure.
   *
   * This should really take a type of
   *   ((cown_ptr<A1>& | cown_ptr<A1>&&)...
   * To get the universal reference type to work, we can't
   * place this constraint on it directly, as it needs to be
   * on a type argument.
   */
  template<typename... Args>
  auto when(Args&&... args)
  {
    return PreWhen(convert_access(std::forward<Args>(args))...);
  }

} // namespace verona::cpp
