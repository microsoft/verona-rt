// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <coroutine>
#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

class Body
{
public:
  int counter;

  ~Body()
  {
    Logging::cout() << "Body destroyed" << Logging::endl;
  }
};

struct promise;

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

void test_body()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto log1 = make_cown<Body>();


  auto coro = [=](acquired_cown<Body> & acq) -> coroutine {

    Logging::cout() << "counter = " << acq->counter << Logging::endl;

    acq->counter++;
    verona::rt::behaviour_yielded = true;
    co_await std::suspend_always{};

    Logging::cout() << "counter = " << acq->counter << Logging::endl;
    Logging::cout() << "end" << Logging::endl;
  };

  coroutine* coro_ptr = new coroutine();

  when(log1) << [coro = std::move(coro), coro_ptr](auto l) mutable {
    
    if (coro_ptr->initialized == false)
    {
      *coro_ptr = std::move(coro(l));
      coro_ptr->resume();
    }
    else
    {
      if (!(coro_ptr->done()))
      {
        coro_ptr->resume();
      }
    }
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  Logging::cout() << "Yield test" << Logging::endl;

  harness.run(test_body);

  return 0;
}