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

  /**
   * Used to track the type of access request by embedding const into
   * the type T, or not having const.
   */
  template<typename T>
  class Access
  {
    ActualCown<std::remove_const_t<T>>* t;
    bool yes_transfer;

  public:
    Access(const cown_ptr<T>& c) : t(c.allocated_cown), yes_transfer(false)
    {
      assert(c.allocated_cown != nullptr);
    }

    Access(cown_ptr<T>&& c) : t(c.allocated_cown), yes_transfer(true)
    {
      assert(c.allocated_cown != nullptr);
      c.allocated_cown = nullptr;
    }

    template<typename F, typename... Args>
    friend class When;
  };

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
    template<typename... Args2>

    friend class Batch;

    /// Set of cowns used by this behaviour.
    std::tuple<Access<Args>...> cown_tuple;

    /// The closure to be executed.
    F f;

    /// Used as a temporary to build the behaviour.
    /// The stack lifetime is tricky, and this avoids
    /// a heap allocation.
    verona::rt::Request requests[sizeof...(Args)];

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

        if (p.yes_transfer)
          requests[index].mark_yes_transfer();

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
          [f = std::move(f), cown_tuple = cown_tuple]() mutable {
            /// Effectively converts ActualCown<T>... to
            /// acquired_cown... .
            auto lift_f = [f = std::move(f)](Access<Args>... args) mutable {
              std::move(f)(access_to_acquired<Args>(args)...);
            };

            std::apply(std::move(lift_f), cown_tuple);
          });
      }
    }

  public:
    When(F&& f_) : f(std::forward<F>(f_)) {}

    When(F&& f_, std::tuple<Access<Args>...> cown_tuple_)
    : f(std::forward<F>(f_)), cown_tuple(cown_tuple_)
    {}

    When(When&& o)
    : cown_tuple(std::move(o.cown_tuple)), f(std::forward<F>(o.f))
    {}

    When(const When&) = delete;
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
    std::tuple<Access<Args>...> cown_tuple;

    PreWhen(Access<Args>... args) : cown_tuple(args...) {}

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
   * Template deduction guide for when.
   */
  template<typename... Args>
  PreWhen(Access<Args>...) -> PreWhen<Args...>;

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
    return PreWhen(Access(std::forward<Args>(args))...);
  }

} // namespace verona::cpp
