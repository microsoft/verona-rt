// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <test/xoroshiro.h>

namespace verona::rt
{
  class PRNG
  {
    xoroshiro::p128r32 rng;

  public:
    PRNG() : rng(5489) {}

    PRNG(uint64_t seed)
    {
      set_seed(seed);
    }

    void set_seed(uint64_t seed)
    {
      rng.set_state(seed);
      // Discard the first 10 values to avoid correlation with the seed.
      // Otherwise, using adjacent seeds will result in poor initial randomness.
      for (size_t i = 0; i < 10; i++)
        rng.next();
    }

    uint32_t next()
    {
      return rng.next();
    }

    uint32_t next(uint32_t max)
    {
      return rng.next() % max;
    }

    uint64_t next64()
    {
      auto top = rng.next();
      auto bottom = rng.next();
      return (static_cast<uint64_t>(top) << 32) | bottom;
    }
  };
} // namespace verona::rt