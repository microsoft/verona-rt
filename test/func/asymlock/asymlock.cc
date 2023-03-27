// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

/**
 * This files contains a collection of simple races on the asymmetric lock.
 * The tests should be run with Thread Sanitizer to detect potential races.
 */

#include <ds/asymlock.h>
#include <thread>

/**
 * Test a basic race between internal and external acquire.
 */
void test_race0()
{
  verona::rt::AsymmetricLock lock;
  size_t protected_value = 0;

  // Internal thread
  auto it = std::thread([&lock, &protected_value]() {
    lock.internal_acquire();
    auto l = protected_value;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    protected_value = l + 1;
    lock.internal_release();
  });

  // External thread
  auto et = std::thread([&lock, &protected_value]() {
    lock.external_acquire();
    auto l = protected_value;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    protected_value = l + 2;
    lock.external_release();
  });

  et.join();
  it.join();

  SNMALLOC_CHECK(protected_value == 3);
}

/**
 * Test racing between internal release and external acquire.
 */
void test_race1()
{
  verona::rt::AsymmetricLock lock;
  size_t protected_value = 0;
  std::atomic<bool> go(false);

  // Internal thread
  auto it = std::thread([&go, &lock, &protected_value]() {
    lock.internal_acquire();
    go = true;
    protected_value++;
    lock.internal_release();
  });

  // External thread
  auto et = std::thread([&go, &lock, &protected_value]() {
    while (!go)
    {
    } // Spin until internal thread has acquired the lock
    lock.external_acquire();
    protected_value++;
    lock.external_release();
  });

  et.join();
  it.join();

  SNMALLOC_CHECK(protected_value == 2);
}

/**
 * Test racing between external release and internal acquire.
 */
void test_race2()
{
  verona::rt::AsymmetricLock lock;
  size_t protected_value = 0;
  std::atomic<bool> go(false);

  // External thread
  auto et = std::thread([&go, &lock, &protected_value]() {
    lock.external_acquire();
    go = true;
    protected_value++;
    lock.external_release();
  });
  // Internal thread
  auto it = std::thread([&go, &lock, &protected_value]() {
    while (!go)
      ;
    lock.internal_acquire();
    protected_value++;
    lock.internal_release();
  });

  et.join();
  it.join();

  SNMALLOC_CHECK(protected_value == 2);
}

int main()
{
  test_race0();
  test_race1();
  test_race2();
  return 0;
}