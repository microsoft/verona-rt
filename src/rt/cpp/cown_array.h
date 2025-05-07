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
   *
   * The template argument owning is used to determine if the cown_array
   * should own the cown_ptr array or not. If it is false, the cown_array
   * will not allocate a new array and will not free it in the destructor.
   */
  template<typename T, bool owning = true>
  struct cown_array
  {
    cown_ptr<T>* array;
    size_t length;

    void constr_helper(cown_ptr<T>* arr)
    {
      if constexpr (owning)
      {
        array = reinterpret_cast<cown_ptr<T>*>(
          heap::alloc(length * sizeof(cown_ptr<T>*)));

        for (size_t i = 0; i < length; i++)
          new (&array[i]) cown_ptr<T>(arr[i]);
      }
      else
      {
        array = arr;
      }
    }

    cown_array(cown_ptr<T>* array_, size_t length_) : length(length_)
    {
      constr_helper(array_);
    }

    cown_array(const cown_array& o)
    {
      length = o.length;
      constr_helper(o.array);
    }

    ~cown_array()
    {
      if constexpr (owning)
      {
        if (array)
        {
          for (size_t i = 0; i < length; i++)
            array[i].~cown_ptr<T>();

          heap::dealloc(array);
        }
      }
    }

    void steal()
    {
      if constexpr (owning)
      {
        heap::dealloc(array);
        array = nullptr;
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
  template<typename T, bool owning>
  class cown_array<const T, owning> : public cown_array<T, owning>
  {
  public:
    cown_array(const cown_array<T, owning>& other) : cown_array<T, owning>(other){};
  };

  template<typename T, bool owning>
  cown_array<const T, owning> read(cown_array<T,owning> cown)
  {
    Logging::cout() << "Read returning const array ptr" << Logging::endl;
    return cown;
  }
}
