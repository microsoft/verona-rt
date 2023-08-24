// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../sched/behaviour.h"
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

    Access(cown_ptr<T>&& c) : t(c.allocated_cown), is_move(true)
    {
      assert(c.allocated_cown != nullptr);
      c.allocated_cown = nullptr;
    }

    template<typename F, typename... Args>
    friend class When;
  };

  /**
   * Used to track the type of access request in the case of cown_array
   * Ownership is handled the same for all cown_ptr in the span.
   * If is_move is true, all cown_ptrs will be moved.
   */
  template<typename T>
  class AccessBatch
  {
    using Type = T;
    ActualCown<std::remove_const_t<T>>** act_array;
    acquired_cown<T>* acq_array;
    size_t arr_len;
    bool is_move;

    void constr_helper(const cown_array<T>& ptr_span)
    {
      // Allocate the actual_cown and the acquired_cown array
      // The acquired_cown array is after the actual_cown one
      size_t act_size =
        ptr_span.length * sizeof(ActualCown<std::remove_const_t<T>>*);
      size_t acq_size =
        ptr_span.length * sizeof(acquired_cown<std::remove_const_t<T>>);
      act_array = reinterpret_cast<ActualCown<std::remove_const_t<T>>**>(
        snmalloc::ThreadAlloc::get().alloc(act_size + acq_size));

      for (size_t i = 0; i < ptr_span.length; i++)
      {
        act_array[i] = ptr_span.array[i].allocated_cown;
      }
      arr_len = ptr_span.length;

      acq_array =
        reinterpret_cast<acquired_cown<T>*>((char*)(act_array) + act_size);

      for (size_t i = 0; i < ptr_span.length; i++)
      {
        new (&acq_array[i]) acquired_cown<T>(*ptr_span.array[i].allocated_cown);
      }
    }

  public:
    AccessBatch(const cown_array<T>& ptr_span) : is_move(false)
    {
      constr_helper(ptr_span);
    }

    AccessBatch(cown_array<T>&& ptr_span) : is_move(true)
    {
      constr_helper(ptr_span);

      ptr_span.length = 0;
      ptr_span.arary = nullptr;
    }

    AccessBatch(AccessBatch&& old)
    {
      act_array = old.act_array;
      acq_array = old.acq_array;
      arr_len = old.arr_len;
      is_move = old.is_move;

      old.acq_array = nullptr;
      old.act_array = nullptr;
      old.arr_len = 0;
    }

    ~AccessBatch()
    {
      if (act_array)
      {
        snmalloc::ThreadAlloc::get().dealloc(act_array);
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
  auto convert_access(const cown_array<T>& c)
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
    // and thus are dynamically allocated.
    // If is_req_extended is true, then req_extended holds an array of Request
    // and the above requests[] array is not used.
    Request* req_extended;
    bool is_req_extended;

    /**
     * This uses template programming to turn the std::tuple into a C style
     * stack allocated array.
     * The index template parameter is used to perform each the assignment for
     * each index.
     */
    template<typename C>
    static void array_assign_helper_access(Request* req, Access<C>& p)
    {
      if constexpr (is_read_only<decltype(p)>())
        *req = Request::read(p.t);
      else
        *req = Request::write(p.t);

      if (p.is_move)
        req->mark_move();

      assert(req->cown() != nullptr);
    }

    template<typename C>
    static size_t
    array_assign_helper_access_batch(Request* req, AccessBatch<C>& p)
    {
      size_t it_cnt = 0;
      for (size_t i = 0; i < p.arr_len; i++)
      {
        if constexpr (is_read_only<decltype(p)>())
          *req = Request::read(p.act_array[i]);
        else
          *req = Request::write(p.act_array[i]);

        if (p.is_move)
          req->mark_move();

        req++;
        it_cnt++;
      }

      return it_cnt;
    }

    template<size_t index = 0>
    size_t array_assign(Request* requests)
    {
      if constexpr (index >= sizeof...(Args))
      {
        return 0;
      }
      else
      {
        size_t it_cnt;

        auto& p = std::get<index>(cown_tuple);
        if constexpr (is_batch<
                        typename std::remove_reference<decltype(p)>::type>())
        {
          it_cnt = array_assign_helper_access_batch(requests, p);
          requests += it_cnt;
        }
        else
        {
          array_assign_helper_access(requests, p);
          requests++;
          it_cnt = 1;
        }
        return it_cnt + array_assign<index + 1>(requests);
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
          to_add = p.arr_len;
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
      return acquired_cown_span<C>{c.acq_array, c.arr_len};
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

        size_t count = array_assign(r);

        return std::make_tuple(
          count,
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
   *   ((cown_ptr<A1>& | cown_ptr<A1>&& | cown_array<A1>& ||
   * cown_array<A1>&& )... To get the universal reference type to work, we
   * can't place this constraint on it directly, as it needs to be on a type
   * argument.
   */
  template<typename... Args>
  auto when(Args&&... args)
  {
    return PreWhen(convert_access(std::forward<Args>(args))...);
  }

} // namespace verona::cpp
