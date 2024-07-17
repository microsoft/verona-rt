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
   * In both cases cown_array has ownership over the cown_ptr
   *
   * The destructor calls the destructor of each cown_ptr and frees the
   * allocated array.
   */
  template<typename T>
  struct cown_array
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
    cown_array(cown_ptr<T>* array_, size_t length_) : length(length_)
    {
      if constexpr (should_move == false)
      {
        constr_helper(array_);
      }
    }

    cown_array(const cown_array& o)
    {
      length = o.length;
      constr_helper(o.array);
    }

    ~cown_array()
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
    cown_array(cown_array&& old) = delete;
    cown_array& operator=(cown_array&&) = delete;
    cown_array& operator=(const cown_array&) = delete;
  };

  /* A cown_array<const T> is used to mark that the cown is being accessed as
   * read-only. (This combines the type as the capability. We do not have deep
   * immutability in C++, so acquired_cown<const T> is an approximation.)
   *
   * We use inheritance to allow us to construct a cown_array<const T> from a
   * cown_array<T>.
   */
  template<typename T>
  class cown_array<const T> : public cown_array<T>
  {
  public:
    cown_array(const cown_array<T>& other) : cown_array<T>(other){};
  };

  template<typename T>
  cown_array<const T> read(cown_array<T> cown)
  {
    Logging::cout() << "Read returning const array ptr" << Logging::endl;
    return cown;
  }
}
