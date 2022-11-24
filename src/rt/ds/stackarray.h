// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstddef>

/**
 * @brief A stack allocated array.
 *
 * Due to portability allocates a largeish array on the stack, if this array is
 * not big enough then dynamically allocates something of the correct size. This
 * is done to avoid the need for a dynamic allocation in the common case.
 */
template<typename T>
class StackArray
{
  // Size of the stack allocated array.
  static constexpr size_t Size = 128;

  // Stack allocated array. Untyped to avoid initialisation and destruction.
  char main[Size * sizeof(T)];

  // Pointer to the array in use, may be main or a dynamically allocated array.
  // If current != &main[0], then current is an owning reference.
  T* current;

  // User requested size.
  std::size_t size;

  T* main_as_T()
  {
    return reinterpret_cast<T*>(&main[0]);
  }

public:
  StackArray(std::size_t size) : size(size)
  {
    if (size > Size)
    {
      current = new T[size]();
    }
    else
    {
      current = main_as_T();
      for (size_t i = 0; i < size; i++)
        new (&current[i]) T();
    }
  }

  ~StackArray()
  {
    if (current != main_as_T())
    {
      delete[] current;
    }
    else
    {
      for (size_t i = 0; i < size; i++)
        current[i].~T();
    }
  }

  /**
   * Return non-owning pointer to the array.
   *
   * Lifetime is managed by the StackArray.
   *
   * TODO C++20:  Use std::span
   */
  T* get()
  {
    return current;
  }

  /**
   * Return non-owning reference to an element of the array.
   */
  T& operator[](std::size_t index)
  {
    assert(index < size);
    return current[index];
  }
};