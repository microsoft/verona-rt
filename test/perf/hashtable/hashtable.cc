// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <debug/harness.h>
#include <memory>
#include <set>

/**
 * This benchmarks test different performance of reader-writer cowns.
 * It creates a hashtable with N buckets (num_buckets). Each behaviour can
 * acquire M buckets (num_dependent_buckets) in read or write mode (rw_ratio /
 * rw_ratio_denom). A behaviour can also spin for extra time to simulate
 * short/long read (read_loop_count) and write (write_loop_count) critical
 * sections. The benchmark creates X behaviours (num_operations) before
 * executing them.
 */

/**
 * This knob controls whether reader-writer properties are checked during
 * behaviour execution. The check ensures that only readers (one or more) or a
 * single writer is running at a given point in time.
 */
#ifndef NDEBUG
#  define DEBUG_RW
#endif

using namespace verona::cpp;

long num_buckets = 16;
long num_dependent_buckets = 8;
long num_entries_per_bucket = 32;
long num_operations = 10'000;
long rw_ratio = 90;
long rw_ratio_denom = 100;
long read_loop_count = 0;
long write_loop_count = 0;

class Entry
{
public:
  union
  {
    size_t val;
    char alignment[64];
  };
  Entry(size_t val) : val(val) {}
};

class Bucket
{
public:
  std::vector<std::shared_ptr<Entry>> list;

  Bucket(std::vector<std::shared_ptr<Entry>> list1) {}

  uint64_t get_addr()
  {
    return (uint64_t)this;
  }
};

thread_local long found_read_ops = 0;
thread_local long not_found_read_ops = 0;
thread_local long found_write_ops = 0;
thread_local long not_found_write_ops = 0;
thread_local long mixed_ops = 0;
thread_local long read_cs_time = 0;
thread_local long write_cs_time = 0;

std::atomic<long> total_found_read_ops = 0;
std::atomic<long> total_not_found_read_ops = 0;
std::atomic<long> total_found_write_ops = 0;
std::atomic<long> total_not_found_write_ops = 0;
std::atomic<long> total_mixed_ops = 0;
std::atomic<long> total_read_cs_time = 0;
std::atomic<long> total_write_cs_time = 0;

#ifndef NDEBUG
auto concurrency = std::make_shared<std::array<std::atomic<size_t>, 1024>>();
#endif

void test_hash_table()
{
  auto t1 = high_resolution_clock::now();
  xoroshiro::p128r32 rand{Systematic::get_prng_next()};

  std::shared_ptr<std::vector<cown_ptr<Bucket>>> buckets =
    std::make_shared<std::vector<cown_ptr<Bucket>>>();

  for (size_t i = 0; i < num_buckets; i++)
  {
    std::vector<std::shared_ptr<Entry>> list;
    for (size_t j = 0; j < (num_entries_per_bucket); j++)
      list.push_back(std::shared_ptr<Entry>(new Entry((num_buckets * j) + i)));
    buckets->push_back(make_cown<Bucket>(list));
  }

  for (size_t i = 0; i < num_operations; i++)
  {
    size_t dependent_buckets = (rand.next() % num_dependent_buckets) + 1;
    std::vector<cown_ptr<Bucket>> read_buckets;
    std::vector<cown_ptr<Bucket>> write_buckets;
    std::set<size_t> reader_idx;
    std::set<size_t> writer_idx;

    for (size_t j = 0; j < dependent_buckets; j++)
    {
      size_t key = rand.next() % (num_buckets * num_entries_per_bucket * 2);
      size_t idx = key % num_buckets;
      if (rand.next() % rw_ratio_denom < rw_ratio)
      {
        read_buckets.push_back((*buckets)[idx]);
        reader_idx.insert(idx);
      }
      else
      {
        write_buckets.push_back((*buckets)[idx]);
        writer_idx.insert(idx);
      }
    }

    for (auto writer : writer_idx)
      reader_idx.erase(writer);

    cown_array<Bucket> readers{
      read_buckets.size() > 0 ? read_buckets.data() : nullptr,
      read_buckets.size()};
    cown_array<Bucket> writers{
      write_buckets.size() > 0 ? write_buckets.data() : nullptr,
      write_buckets.size()};

    when(read(readers), writers) << [reader_idx, writer_idx](
                                      acquired_cown_span<const Bucket> readers,
                                      acquired_cown_span<Bucket> writers) {
      Logging::cout() << "Num readers: " << readers.length
                      << " Num writers: " << writers.length << Logging::endl;
#ifdef DEBUG_RW
      for (auto reader : reader_idx)
      {
        auto val = (*concurrency)[reader].fetch_add(2);
        Logging::cout() << "Reader_idx " << reader << " rcount " << val
                        << Logging::endl;
        check(val % 2 == 0);
      }
      for (auto writer : writer_idx)
      {
        auto val = (*concurrency)[writer].fetch_add(1);
        Logging::cout() << "Writer_idx " << writer << " rcount " << val
                        << Logging::endl;
        check(val == 0);
      }
#endif

      found_read_ops += readers.length;
      found_write_ops += writers.length;
      mixed_ops++;

      for (volatile int i = 0; i < read_loop_count * readers.length; i++)
        Aal::pause();
      for (volatile int i = 0; i < write_loop_count * writers.length; i++)
        Aal::pause();

#ifdef DEBUG_RW
      for (auto reader : reader_idx)
      {
        auto val = (*concurrency)[reader].fetch_add(-2);
        Logging::cout() << "Reader_idx " << reader << " rcount " << val
                        << Logging::endl;
        check((val >= 2) && (val % 2 == 0));
      }
      for (auto writer : writer_idx)
      {
        auto val = (*concurrency)[writer].fetch_add(-1);
        Logging::cout() << "Writer_idx " << writer << " rcount " << val
                        << Logging::endl;
        check(val == 1);
      }
#endif
    };
  }

  auto t2 = high_resolution_clock::now();
  auto ns_int = duration_cast<nanoseconds>(t2 - t1);
  auto us_int = duration_cast<microseconds>(t2 - t1);
  auto ms_int = duration_cast<milliseconds>(t2 - t1);
  std::cout << "Behaviour generation Elapsed time: " << ms_int.count() << "ms "
            << us_int.count() << "us " << ns_int.count() << "ns" << std::endl;

  std::cout << "Total ops: "
            << (total_found_read_ops.load() + total_found_write_ops.load() +
                total_not_found_read_ops.load() +
                total_not_found_write_ops.load())
            << " found read ops: " << total_found_read_ops.load()
            << " not found read ops: " << total_not_found_read_ops.load()
            << " found write ops: " << total_found_write_ops.load()
            << " not found write ops: " << total_not_found_write_ops.load()
            << std::endl;
}

void finish(void)
{
  std::stringstream ss;
  ss << "Thread: " << std::this_thread::get_id()
     << " found read ops: " << found_read_ops
     << " not found read ops: " << not_found_read_ops
     << " found write ops: " << found_write_ops
     << " not found write ops: " << not_found_write_ops
     << " mixed ops: " << mixed_ops << "\n";
  total_found_read_ops.fetch_add(found_read_ops);
  total_not_found_read_ops.fetch_add(not_found_read_ops);
  total_found_write_ops.fetch_add(found_write_ops);
  total_not_found_write_ops.fetch_add(not_found_write_ops);
  total_mixed_ops.fetch_add(mixed_ops);

  total_read_cs_time.fetch_add(read_cs_time);
  total_write_cs_time.fetch_add(write_cs_time);
  std::cout << ss.str();
}

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);

  num_buckets = opt.is<size_t>("--num_buckets", num_buckets);
  num_dependent_buckets =
    opt.is<size_t>("--num_dependent_buckets", num_dependent_buckets);
  num_entries_per_bucket =
    opt.is<size_t>("--num_entries_per_bucket", num_entries_per_bucket);
  num_operations = opt.is<size_t>("--num_operations", num_operations);
  rw_ratio = opt.is<size_t>("--rw_ratio", rw_ratio);
  rw_ratio_denom = opt.is<size_t>("--rw_ratio_denom", rw_ratio_denom);
  read_loop_count = opt.is<size_t>("--read_loop_count", read_loop_count);
  write_loop_count = opt.is<size_t>("--write_loop_count", write_loop_count);

  check(num_dependent_buckets <= num_buckets);

#ifdef DEBUG_RW
  check(num_buckets <= 1024);
#endif

  SystematicTestHarness harness(argc, argv);

  harness.run_at_termination = finish;

  auto t1 = high_resolution_clock::now();
  harness.run(test_hash_table);
  auto t2 = high_resolution_clock::now();

#ifdef DEBUG_RW
  for (int i = 0; i < num_buckets; i++)
    check((*concurrency)[i].load() == 0);
#endif

  std::cout << "Num buckets: " << num_buckets
            << " Num dependent buckets: " << num_dependent_buckets
            << " Num entries per bucket: " << num_entries_per_bucket
            << " Num operations: " << num_operations
            << " Read write ratio readers: " << rw_ratio
            << " out of total: " << rw_ratio_denom
            << " Read loop count: " << read_loop_count
            << " Write loop count: " << write_loop_count << std::endl;

  std::cout << "Total ops: " << total_mixed_ops.load()
            << " found read ops: " << total_found_read_ops.load()
            << " not found read ops: " << total_not_found_read_ops.load()
            << " found write ops: " << total_found_write_ops.load()
            << " not found write ops: " << total_not_found_write_ops.load()
            << std::endl;

  std::cout << "Avg Read CS time: "
            << ((double)total_read_cs_time.load()) /
      (total_found_read_ops.load() + total_not_found_read_ops.load())
            << " ns" << std::endl;
  std::cout << "Avg Write CS time: "
            << ((double)total_write_cs_time.load()) /
      (total_found_write_ops.load() + total_not_found_write_ops.load())
            << " ns" << std::endl;

  auto ns_int = duration_cast<nanoseconds>(t2 - t1);
  auto us_int = duration_cast<microseconds>(t2 - t1);
  auto ms_int = duration_cast<milliseconds>(t2 - t1);
  std::cout << "Elapsed time: " << ms_int.count() << "ms " << us_int.count()
            << "us " << ns_int.count() << "ns" << std::endl;
}
