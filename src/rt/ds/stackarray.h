// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <type_traits>

template<typename T>
class StackArray
{
    T main[128];
    T* current;

  public:
    StackArray(size_t size)
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

    size_t& operator[](size_t index)
    {
        return current[index];
    }
};