// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <ctime>
#include <debug/harness.h>
#include <test/opt.h>
#include <verona.h>

using namespace snmalloc;
using namespace verona::rt;
using namespace verona::cpp;

static constexpr int start_count = 1'00'000;
struct A
{
  int id;
  int count = start_count;
  clock_t begin;

  A(int id_) : id{id_} {}
};

int constexpr n_cowns = 6;
double elapsed_secs[n_cowns];

void loop(cown_ptr<A> c)
{
  when(c) << [c = std::move(c)](auto a) {
    auto& count = a->count;
    auto id = a->id;

    if (count == start_count)
    {
      a->begin = clock();
    }

    if (count == 0)
    {
      clock_t end = clock();
      double elapsed_second = double(end - a->begin) / CLOCKS_PER_SEC;
      elapsed_secs[id] = elapsed_second;
      // printf("%d: %f\n", a->id, elapsed_second);
      return;
    }

    count--;
    loop(std::move(c));
  };
}

void spawn()
{
  when() << []() {
    for (int i = 0; i < n_cowns; ++i)
    {
      loop(make_cown<A>(i));
    }
  };
}

void assert_variance()
{
  using namespace std;
  auto result = minmax_element(elapsed_secs, elapsed_secs + n_cowns);
  auto min = *result.first;
  auto max = *result.second;
  check(min != 0 && max != 0);
  // printf("%f\n", (max - min)/max);
  // variance should be less than 15%
  for (int i = 0; i < n_cowns; i++)
  {
    printf("cown[%d] took %f\n", i, elapsed_secs[i]);
  }
  if ((max - min) / max > 0.15)
  {
    printf("(max - min) / max = %f\n", (max - min) / max);
    printf("variance too large");
    check(false);
  }
}

int main()
{
#ifdef USE_SYSTEMATIC_TESTING
  std::cout << "This test does not make sense to run systematically."
            << std::endl;
#else
  size_t cores = 2;
  Scheduler& sched = Scheduler::get();
  sched.init(cores);
  sched.set_fair(true);

  spawn();

  sched.run();
  snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  assert_variance();

  puts("done");
#endif
  return 0;
}
