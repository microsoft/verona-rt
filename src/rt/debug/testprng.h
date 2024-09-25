// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "ds/prng.h"

#include <random>

struct TestPRNG
{
#ifdef USE_SYSTEMATIC_TESTING
  // Use xoroshiro for systematic testing, because it's simple and
  // and deterministic across platforms.
  verona::rt::PRNG rand;
#else
  // We don't mind data races for our PRNG, because concurrent testing means
  // our results will already be nondeterministic. However, data races may
  // cause xoroshiro to abort.
  std::mt19937_64 rand;
#endif

  TestPRNG(size_t seed) : rand(seed) {}

  uint64_t next()
  {
#ifdef USE_SYSTEMATIC_TESTING
    return rand.next();
#else
    return rand();
#endif
  }

  void seed(size_t seed)
  {
#ifdef USE_SYSTEMATIC_TESTING
    return rand.set_seed(seed);
#else
    return rand.seed(seed);
#endif
  }
};