// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <debug/harness.h>
#include <ds/scramble.h>

struct Fork : public VCown<Fork>
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

struct Ping
{
  void operator()() {}
};

/**
 * This Message holds on to the only reference to a Cown, that it
 * will "Ping" once it is delivered.  This is used to find missing
 * scans of messages.  If a message is not scanned, the Cown c
 * will be reclaimable, once this message is delivered it will be
 * deallocated.
 **/
struct KeepAlive
{
  Cown* c;

  KeepAlive()
  {
    c = new Fork(999);
    schedule_lambda(c, Ping());
  }

  void trace(ObjectStack& fields) const
  {
    fields.push(c);
  }

  void operator()()
  {
    schedule_lambda<YesTransfer>(c, Ping());
  }
};

struct Philosopher : public VCown<Philosopher>
{
  size_t id;
  std::vector<Cown*> forks;
  size_t to_eat;

  Philosopher(size_t id_, std::vector<Cown*> forks_, size_t to_eat_)
  : id(id_), forks(forks_), to_eat(to_eat_)
  {}

  void trace(ObjectStack& fields) const
  {
    for (auto f : forks)
    {
      fields.push(f);
    }
  }
};

void eat_send(Philosopher* p);

struct Ponder
{
  Philosopher* p;

  Ponder(Philosopher* p) : p(p) {}

  void operator()()
  {
    Logging::cout() << "Philosopher " << p->id << " " << p << " pondering "
                    << p->to_eat << std::endl;
    eat_send(p);
  }
};

struct Eat
{
  Philosopher* eater;

  void operator()()
  {
    Logging::cout() << "Philosopher " << eater->id << " " << eater
                    << " eating (" << this << ")" << std::endl;
    for (auto f : eater->forks)
    {
      ((Fork*)f)->uses++;
    }

    schedule_lambda(eater, Ponder(eater));
  }

  Eat(Philosopher* p_) : eater(p_)
  {
    Logging::cout() << "Eat Message " << this << " for Philosopher " << p_->id
                    << " " << p_ << std::endl;
  }

  void trace(ObjectStack& fields) const
  {
    Logging::cout() << "Calling custom trace" << std::endl;
    fields.push(eater);
  }
};

void eat_send(Philosopher* p)
{
  if (p->to_eat == 0)
  {
    auto& alloc = ThreadAlloc::get();
    Logging::cout() << "Releasing Philosopher " << p->id << " " << p
                    << std::endl;
    Cown::release(alloc, p);
    return;
  }

  p->to_eat--;
  schedule_lambda(p->forks.size(), p->forks.data(), Eat(p));

  schedule_lambda(p->forks[0], KeepAlive());
}

void test_dining(
  size_t philosophers,
  size_t hunger,
  size_t fork_count,
  SystematicTestHarness* h)
{
  std::vector<Fork*> forks;
  for (size_t i = 0; i < philosophers; i++)
  {
    auto f = new Fork(i);
    forks.push_back(f);
    Logging::cout() << "Fork " << i << " " << f << std::endl;
  }

  verona::Scramble scrambler;
  xoroshiro::p128r32 rand(h->current_seed());

  for (size_t i = 0; i < philosophers; i++)
  {
    scrambler.setup(rand);

    std::vector<Cown*> my_forks;

    std::sort(forks.begin(), forks.end(), [&scrambler](Fork*& a, Fork*& b) {
      return scrambler.perm(((Cown*)a)->id()) <
        scrambler.perm(((Cown*)b)->id());
    });

    for (size_t j = 0; j < fork_count; j++)
    {
      forks[j]->uses_expected += hunger;
      Cown::acquire(forks[j]);
      my_forks.push_back(forks[j]);
    }

    auto p = new Philosopher(i, my_forks, hunger);
    schedule_lambda(p, Ponder(p));
    Logging::cout() << "Philosopher " << i << " " << p << std::endl;
    for (size_t j = 0; j < fork_count; j++)
    {
      Logging::cout() << "   Fork " << ((Fork*)my_forks[j])->id << " "
                      << my_forks[j] << std::endl;
    }
  }

  for (size_t i = 0; i < philosophers; i++)
  {
    Cown::release(ThreadAlloc::get(), forks[i]);
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
