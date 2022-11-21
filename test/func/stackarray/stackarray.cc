// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <iostream>
#include <snmalloc/snmalloc.h>
#include <verona.h>

using namespace verona::rt;

size_t d;
size_t c;

struct Destructor
{
    ~Destructor()
    {
        d++;
    }
};

struct Constructor
{
    Constructor()
    {
        c++;
    }
};

struct Both
{
    Destructor d;
    Constructor c;
};

// Test constructors are called
void test_c(size_t i)
{
    StackArray<Constructor> a(i);
    if(c != i)
        abort();
    c = 0;
    std::cout << "." << std::flush;
}

// Test destructors are called
void test_d(size_t i)
{
    {
        StackArray<Destructor> a(i);
        if(d != 0)
            abort();
    }
    if(d != i)
        abort();
    d = 0;
    std::cout << "." << std::flush;
}

// Test constructors and destructors are called
void test_both(size_t i)
{
    {
        StackArray<Both> a(i);
        if(c != i)
            abort();
        if(d != 0)
            abort();
        c = 0;
    }
    if(d != i)
        abort();
    d = 0;
    std::cout << "." << std::flush;
}

// Test primitives are initialised
void test_size_t(size_t i)
{
    StackArray<size_t> a(i);
    for(size_t j = 0; j < i; j++)
        if (a[j] != 0)
            abort();
    std::cout << "." << std::flush;
}

int main()
{
    test_c(10);
    test_d(10);
    test_both(10);
    test_size_t(10);

    test_c(200);
    test_d(200);
    test_both(200);
    test_size_t(200);

    std::cout << std::endl;
}

