// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

namespace verona::cpp
{

  /**
   * This is a span of cown_ptr that has ownership of the included cown_ptrs
   *
   * The default constructor takes an array of cown_ptr and heap allocates
   * another array to hold those pointers after incrementing the reference
   * count. Alternatively, the constructor takes a template argument to move
   * the cown_ptr array and avoid the allocation.
   * In both cases cown_ptr_span has ownership over the cown_ptr
   *
   * The destructor calls the destructor of each cown_ptr and frees the
   * allocated array.
   */
  template<typename T>
  struct cown_ptr_span
  {
    cown_ptr<T>* array;
    size_t length;

    void constr_helper(cown_ptr<T>* arr)
    {
      array = reinterpret_cast<cown_ptr<T>*>(
        snmalloc::ThreadAlloc::get().alloc(length * sizeof(cown_ptr<T>*)));

      for (size_t i = 0; i < length; i++)
        new (&array[i]) cown_ptr<T>(arr[i]);
    }

    template<bool should_move = false>
    cown_ptr_span(cown_ptr<T>* array_, size_t length_) : length(length_)
    {
      if constexpr (should_move == false)
      {
        constr_helper(array_);
      }
    }

    cown_ptr_span(const cown_ptr_span& o)
    {
      length = o.length;
      constr_helper(o.array);
    }

    ~cown_ptr_span()
    {
      if (array)
      {
        for (size_t i = 0; i < length; i++)
          array[i].~cown_ptr<T>();

        snmalloc::ThreadAlloc::get().dealloc(array);
      }
    }

    // Not needed at the moment. Marked as delete to avoid bugs
    // Could be implemented if necessary
    cown_ptr_span(cown_ptr_span&& old) = delete;
    cown_ptr_span& operator=(cown_ptr_span&&) = delete;
    cown_ptr_span& operator=(const cown_ptr_span&) = delete;
  };
}
