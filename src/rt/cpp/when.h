// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "cown.h"
#include "../sched/behaviour.h"

#include <functional>
#include <tuple>
#include <utility>
#include <verona.h>

namespace verona::cpp
{
  using namespace verona::rt;

  /**
   * Used to track the type of access request by embedding const into
   * the type T, or not having const.
   */
  template<typename T>
  class Access
  {
    ActualCown<std::remove_const_t<T>>* t;

  public:
    Access(const cown_ptr<T>& c) : t(c.allocated_cown) {}

    template<typename F, typename... Args>
    friend class WhenBuilder;
  };

  template<typename... Args>
  class WhenBuilderBatch
  {
    std::tuple<Args&&...> when_batch;

    template<size_t index = 0>
    void mark_as_batch(void)
    {
      if constexpr (index >= sizeof...(Args))
      {
        return;
      }
      else
      {
        auto&& w = std::get<index>(when_batch);
        w.part_of_batch = true;

        static_assert(
          std::tuple_size<decltype(w.cown_tuple)>{} > 0,
          "It does not make sense to atomically schedule a behaviour without a "
          "cown dependency");

        mark_as_batch<index + 1>();
      }
    }

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
          std::get<0>(t), std::get<1>(t), std::get<2>(t));
        create_behaviour<index + 1>(barray);
      }
    }

  public:
    WhenBuilderBatch(Args&&... args) : when_batch(std::forward<Args>(args)...)
    {
      mark_as_batch();
    }

    WhenBuilderBatch(const WhenBuilderBatch&) = delete;

    ~WhenBuilderBatch()
    {
      BehaviourCore* barray[sizeof...(Args)];
      create_behaviour(barray);

      BehaviourCore::schedule_many(barray, sizeof...(Args));
    }

    // FIXME: Overload + operator for WhenBuilderBatch + WhenBuilder
  };

  template<typename F, typename... Args>
  class WhenBuilder
  {
    template<class T>
    struct is_read_only : std::false_type
    {};
    template<class T>
    struct is_read_only<Access<const T>> : std::true_type
    {};
    template<typename... Args2>

    friend class WhenBuilderBatch;

    std::tuple<Access<Args>...> cown_tuple;
    F f;
    verona::rt::Request requests[sizeof...(Args)];
    bool part_of_batch;

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
        auto p = std::get<index>(cown_tuple);
        if constexpr (is_read_only<decltype(p)>())
          requests[index] = Request::read(p.t);
        else
          requests[index] = Request::write(p.t);
        assert(requests[index].cown() != nullptr);
        array_assign<index + 1>(requests);
      }
    }

    /**
     * Converts a single `cown_ptr` into a `acquired_cown`.
     *
     * Needs to be a separate function for the template parameter to work.
     */
    template<typename C>
    static acquired_cown<C> access_to_acquired(Access<C> c)
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
        array_assign(requests);

        return std::make_tuple(
          sizeof...(Args),
          requests,
          [f = std::forward<F>(f), cown_tuple = cown_tuple]() mutable {
            /// Effectively converts ActualCown<T>... to
            /// acquired_cown... .
            auto lift_f = [f =
                             std::forward<F>(f)](Access<Args>... args) mutable {
              f(access_to_acquired<Args>(args)...);
            };

            std::apply(lift_f, cown_tuple);
          });
      }
    }

  public:
    WhenBuilder(F&& f_) : f(std::move(f_)), part_of_batch(false) {}

    WhenBuilder(F&& f_, std::tuple<Access<Args>...> cown_tuple_)
    : f(std::move(f_)), cown_tuple(cown_tuple_), part_of_batch(false)
    {}

    WhenBuilder(WhenBuilder&& o)
    : cown_tuple(std::move(o.cown_tuple)), f(std::move(o.f))
    {}

    WhenBuilder(const WhenBuilder&) = delete;

    ~WhenBuilder()
    {
      if (part_of_batch)
        return;

      if constexpr (sizeof...(Args) == 0)
      {
        verona::rt::schedule_lambda(std::forward<F>(f));
      }
      else
      {
        verona::rt::Request requests[sizeof...(Args)];
        array_assign(requests);

        verona::rt::schedule_lambda(
          sizeof...(Args),
          requests,
          [f = std::forward<F>(f), cown_tuple = cown_tuple]() mutable {
            /// Effectively converts ActualCown<T>... to
            /// acquired_cown... .
            auto lift_f = [f =
                             std::forward<F>(f)](Access<Args>... args) mutable {
              f(access_to_acquired<Args>(args)...);
            };

            std::apply(lift_f, cown_tuple);
          });
      }
    }

    template<typename B>
    auto operator+(B&& wb)
    {
      return WhenBuilderBatch(std::move(*this), std::move(wb));
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
  class When
  {
    // Note only requires friend when Args2 == Args
    // but C++ doesn't like this.
    template<typename... Args2>
    friend auto when(Args2&&... args);

    /**
     * Internally uses AcquiredCown.  The cown is only acquired after the
     * behaviour is scheduled.
     */
    std::tuple<Access<Args>...> cown_tuple;

    When(Access<Args>... args) : cown_tuple(args...) {}

  public:
    template<typename F>
    WhenBuilder<F, Args...> operator<<(F&& f)
    {
      Scheduler::stats().behaviour(sizeof...(Args));

      if constexpr (sizeof...(Args) == 0)
      {
        return WhenBuilder(std::forward<F>(f));
      }
      else
      {
        return WhenBuilder(std::move(f), std::move(cown_tuple));
      }
    }

    ~When() {}
  };

  /**
   * Template deduction guide for when.
   */
  template<typename... Args>
  When(Access<Args>...)->When<Args...>;

  /**
   * Template deduction guide for Access.
   */
  template<typename T>
  Access(const cown_ptr<T>&)->Access<T>;

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
    return When(Access(args)...);
  }
} // namespace verona::cpp
