// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <iostream>

#ifdef COROUTINES
#  ifdef EXPERIMENTAL_CORO
#    include <experimental/coroutine>
#  else
#    include <coroutine>
#  endif
#endif

#include "../sched/behaviour.h"

namespace verona::cpp
{
#ifndef COROUTINES
  struct coroutine
  {};
#else

#  ifdef EXPERIMENTAL_CORO
  using namespace std::experimental;
#  else
  using namespace std;
#  endif

  struct coroutine
  {
    struct promise_type;
    using handle_type = coroutine_handle<promise_type>;

    struct promise_type
    {
      coroutine get_return_object()
      {
        return {handle_type::from_promise(*this)};
      }
      suspend_always initial_suspend() noexcept
      {
        return {};
      }
      suspend_always final_suspend() noexcept
      {
        return {};
      }
      void unhandled_exception() {}
      void return_void() {}
    };

    handle_type h_;
    bool initialized = false;

    coroutine(handle_type h) : h_(h), initialized(true) {}
    coroutine() : h_(nullptr), initialized(false) {}

    void resume()
    {
      h_.resume();
    }

    bool done() const
    {
      return h_.done();
    }

    void destroy()
    {
      h_.destroy();
    }
  };

  template<typename F>
  auto prepare_coro_lambda(F&& f)
  {
    coroutine coro_state;

    auto coro_f = [f = std::move(f),
                   coro_state = std::move(coro_state)](auto... args) mutable {
      if (coro_state.initialized == false)
      {
        coro_state = std::move(f(args...));
      }

      coro_state.resume();

      if (!(coro_state.done()))
      {
        verona::rt::Behaviour::behaviour_rerun() = true;
      }
      else
      {
        coro_state.destroy();
      }
    };

    return coro_f;
  }
#endif
} // namespace verona::cpp
