// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

// WrapIndex is a simple class that wraps around an index.
template<size_t N>
class WrapIndex
{
  size_t index;

public:
  WrapIndex() : index(0) {}

  // Returns the next index and wraps around.
  size_t operator++()
  {
    index = (index + 1) % N;
    return index;
  }

  size_t operator--(int)
  {
    auto result = index;
    if (result == 0)
      index = N - 1;
    else
      index--;
    return result;
  }

  operator size_t() const
  {
    return index;
  }
};
