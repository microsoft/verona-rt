// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <random>
#include <test/xoroshiro.h>

namespace verona::rt
{
  // Template parameter expresses if the context is multi-threaded
  template<bool Multithreaded = false>
  class PRNG
  {
    static constexpr bool ThreadSafeRequired =
#ifdef USE_SYSTEMATIC_TESTING
      // If we are using systematic testing, then the PRNG does not need to be
      // thread safe, even when called by multiple threads
      false;
#else
      Multithreaded;
#endif

    std::conditional_t<ThreadSafeRequired, std::mt19937_64, xoroshiro::p128r32>
      rng;

    void set_seed(std::mt19937_64& r, uint64_t seed)
    {
      r.seed(seed);
    }

    void set_seed(xoroshiro::p128r32& r, uint64_t seed)
    {
      r.set_state(seed);
    }

    uint32_t next(std::mt19937_64& r)
    {
      return r();
    }

    uint32_t next(xoroshiro::p128r32& r)
    {
      return r.next();
    }

  public:
    PRNG() : rng(5489) {}

    PRNG(uint64_t seed)
    {
      set_seed(seed);
    }

    void set_seed(uint64_t seed)
    {
      set_seed(rng, seed);
      // Discard the first 10 values to avoid correlation with the seed.
      // Otherwise, using adjacent seeds will result in poor initial
      // randomness.
      for (size_t i = 0; i < 10; i++)
        next();
    }

    uint32_t next()
    {
      return next(rng);
    }

    uint32_t next(uint32_t max)
    {
      return next() % max;
    }

    uint64_t next64()
    {
      auto top = next();
      auto bottom = next();
      return (static_cast<uint64_t>(top) << 32) | bottom;
    }
  };
} // namespace verona::rt