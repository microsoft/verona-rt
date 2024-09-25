// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <ds/prng.h>
#include <ds/scramble.h>
#include <snmalloc/snmalloc.h>
#include <test/opt.h>

// Simple test that perm is not preserving orders.
int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);
  const auto seed = opt.is<size_t>("--seed", snmalloc::Aal::tick());

  verona::rt::PRNG<> r(seed);
  size_t total = 0;
  size_t square_diff_total = 0;
  constexpr size_t n = 100;
  constexpr size_t s = 1000;
  for (size_t i = 0; i < n; i++)
  {
    verona::rt::Scramble s1;
    s1.setup(r);

    verona::rt::Scramble s2;
    s2.setup(r);

    size_t count = 0;

    for (uintptr_t p = 0; p < s; p++)
    {
      if ((s1.perm(p) < s1.perm(p + 1)) == (s2.perm(p) < s2.perm(p + 1)))
        count++;
    }

    total += count;
    auto abs_diff = count > (s / 2) ? count - (s / 2) : (s / 2) - count;
    square_diff_total += abs_diff * abs_diff;
  }

  auto mean = total / n;
  auto variance = square_diff_total / n;

  bool failed = false;

  if (mean > (s / 2) + 10)
  {
    failed = true;
    std::cout << "Mean is too high" << std::endl;
  }

  if (mean < (s / 2) - 10)
  {
    failed = true;
    std::cout << "Mean is too low" << std::endl;
  }

  if (variance > s)
  {
    failed = true;
    std::cout << "Variance is too high" << std::endl;
  }

  if (variance < s / 5)
  {
    failed = true;
    std::cout << "Variance is too low" << std::endl;
  }

  if (failed)
  {
    std::cout << "Test failed" << std::endl;
    std::cout << "    " << argv[0] << " --seed " << seed << std::endl;
    std::cout << "Mean: " << mean << std::endl;
    std::cout << "Variance: " << variance << std::endl;

    return 1;
  }
  else
    return 0;
}
