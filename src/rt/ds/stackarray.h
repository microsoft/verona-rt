// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstddef>

template<typename T>
class StackArray
{
  T main[128];
  T* current;

public:
  StackArray(std::size_t size)
  {
    if (size <= 128)
    {
      current = main;
    }
    else
    {
      current = new T[size];
    }
  }

  ~StackArray()
  {
    if (current != main)
    {
      delete[] current;
    }
  }

  T* get()
  {
    return current;
  }

  std::size_t& operator[](std::size_t index)
  {
    return current[index];
  }
};