// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

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

  public:
    Access(const cown_ptr<T>& c) : t(c.allocated_cown) {}

    template<typename... Args>
    friend class When;
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
        mark_as_batch<index + 1>();
      }
    }

    public:
    WhenBuilderBatch(Args&&... args) : when_batch(std::forward<Args>(args)...)
    {
      std::cout << "Calling when builder batch constructor\n";
      mark_as_batch();
    }

    WhenBuilderBatch(const WhenBuilderBatch&) = delete;

    ~WhenBuilderBatch()
    {
      std::cout << "Calling when builder batch destructor\n";
    }

    // FIXME: Overload + operator for WhenBuilderBatch + WhenBuilder
  };

  template<typename F, typename... Args>
  class WhenBuilder
  {
    template<typename... Args2>
    friend class WhenBuilderBatch;

    std::tuple<Access<Args>...> cown_tuple;
    F f;
    bool part_of_batch;

  public:
    WhenBuilder(F&& f_) : f(f_), part_of_batch(false)
    {
      std::cout << "Calling when builder constructor fn\n";
    }

    WhenBuilder(F&& f_, std::tuple<Access<Args>...> cown_tuple_)
    : f(f_), cown_tuple(cown_tuple_), part_of_batch(false)
    {
      std::cout << "Calling when builder constructor complex\n";
    }

    WhenBuilder(WhenBuilder&& o) : cown_tuple(std::move(o.cown_tuple)), f(std::move(o.f))
    {
      std::cout << "Calling when builder move constructor\n";
    }

    WhenBuilder(const WhenBuilder&) = delete;

    ~WhenBuilder()
    {
      std::cout << "Calling when builder destructor\n";
      if (part_of_batch)
        std::cout << "part of batch. Don't do anything\n";
      else
        std::cout << "Not part of batch. Do something\n";

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
    template<class T>
    struct is_read_only : std::false_type
    {};
    template<class T>
    struct is_read_only<Access<const T>> : std::true_type
    {};

    // Note only requires friend when Args2 == Args
    // but C++ doesn't like this.
    template<typename... Args2>
    friend auto when(Args2&&... args);

    /**
     * Internally uses AcquiredCown.  The cown is only acquired after the
     * behaviour is scheduled.
     */
    std::tuple<Access<Args>...> cown_tuple;

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

    When(Access<Args>... args) : cown_tuple(args...) {}

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

  public:
    /**
     * Applies the closure to schedule the behaviour on the set of cowns.
     */
    template<typename F>
    void operator<<(F&& f)
    {
      Scheduler::stats().behaviour(sizeof...(Args));

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

    template<typename F>
    WhenBuilder<F, Args...> operator>>(F&& f)
    {
      Scheduler::stats().behaviour(sizeof...(Args));

      if constexpr (sizeof...(Args) == 0)
      {
        std::cout << "Construct empty when builder\n";
        return WhenBuilder(std::move(f));
      }
      else
      {
        std::cout << "Construct non-empty when builder\n";
#if 0
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
#endif
        return WhenBuilder(std::move(f), std::move(cown_tuple));
      }
    }

    ~When()
    {
      std::cout << "Destructor run\n";
    }
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
