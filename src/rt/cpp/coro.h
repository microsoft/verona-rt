// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#ifdef COROUTINES
#include <coroutine>
#include <iostream>
#endif

#include "../sched/behaviour.h"

namespace verona::cpp
{
#ifndef COROUTINES
    struct coroutine{};
#else
  struct coroutine
  {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type
    {
      coroutine get_return_object()
      {
        return {handle_type::from_promise(*this)};
      }
      std::suspend_always initial_suspend() noexcept
      {
        return {};
      }
      std::suspend_always final_suspend() noexcept
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

    void resume() const
    {
      h_.resume();
    }

    bool done() const
    {
      return h_.done();
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
    };

    return coro_f;
  }
#endif
} // namespace verona::cpp
