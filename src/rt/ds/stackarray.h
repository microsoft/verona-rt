// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstddef>

/**
 * @brief A stack allocated array.
 * 
 * Due to portability allocates a largeish array on the stack, if this array is not
 * big enough then dynamically allocates something of the correct size. This is done 
 * to avoid the need for a dynamic allocation in the common case.
 */
template<typename T>
class StackArray
{
  // Size of the stack allocated array.
  static constexpr size_t Size = 128;

  // Stack allocated array.
  T main[Size];

  // Pointer to the array in use, may be main or a dynamically allocated array.
  T* current;

  // User requested size. 
  std::size_t size;

public:
  StackArray(std::size_t size) : current(main), size(size)
  {
    if (size > Size)
      current = new T[size];
  }

  ~StackArray()
  {    
    if (current != main)
      delete[] current;
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