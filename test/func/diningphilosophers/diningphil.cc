// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <debug/harness.h>
#include <ds/scramble.h>

using namespace verona::cpp;
struct Fork
{
  size_t id;
  size_t uses_expected;
  size_t uses;

  Fork(size_t id) : id(id), uses_expected(0), uses(0){};

  ~Fork()
  {
    check(uses_expected == uses);
  }
};

void eat(size_t id, std::vector<cown_ptr<Fork>> forks, size_t to_eat)
{
  if (to_eat == 0)
  {
    Logging::cout() << "Releasing Philosopher " << id << std::endl;
    return;
  }
  cown_array<Fork, false> forkspan(forks.data(), forks.size());
  when(forkspan, [id, forks, to_eat](auto f) {
    Logging::cout() << "Philosopher " << id << " eating " << to_eat
                    << std::endl;
    for (size_t i = 0; i < f.length(); i++)
    {
      Logging::cout() << "Fork " << f[i].cown() << " " << f[i]->id << std::endl;
      f[i]->uses++;
    }

    eat(id, forks, to_eat - 1);

    // KeepAlive
    when(forks[0], [](auto) {});
  });
}

void test_dining(
  size_t philosophers,
  size_t hunger,
  size_t fork_count,
  SystematicTestHarness* h)
{
  std::vector<cown_ptr<Fork>> forks;
  for (size_t i = 0; i < philosophers; i++)
  {
    forks.push_back(make_cown<Fork>(i));
    Logging::cout() << "Fork " << i << " " << forks[i] << std::endl;
  }

  verona::rt::PRNG<> rand{h->current_seed()};

  for (size_t i = 0; i < philosophers; i++)
  {
    std::vector<cown_ptr<Fork>> my_forks;

    for (size_t j = 0; j < fork_count; j++)
    {
      size_t fork_idx = rand.next64() % philosophers;
      my_forks.push_back(forks[fork_idx]);
      when(forks[fork_idx], [=](auto f) { f->uses_expected += hunger; });
    }

    eat(i, std::move(my_forks), hunger);
  }
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  size_t phil = harness.opt.is<size_t>("--philosophers", 4);
  std::cout << " --philosophers " << phil << std::endl;
  size_t hunger = harness.opt.is<size_t>("--hunger", 4);
  std::cout << " --hunger " << hunger << std::endl;
  size_t forks = harness.opt.is<size_t>("--forks", 2);
  std::cout << " --forks " << forks << std::endl;

  if (forks > phil)
  {
    phil = forks;
    std::cout << " overriding philosophers as need as many as forks."
              << std::endl;
  }

  harness.run(test_dining, phil, hunger, forks, &harness);

  return 0;
}
