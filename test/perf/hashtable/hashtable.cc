// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <debug/harness.h>
#include <memory>

using namespace verona::cpp;

long num_buckets = 1;
long num_entries_per_bucket = 128;
long num_operations = 100'000'000;
long rw_ratio = 90; // X% readers
long read_loop_count = 100;
long write_loop_count = 100;

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
  std::vector<Entry*> list;

  Bucket(std::vector<Entry*> list) : list(list) {}

  uint64_t get_addr()
  {
    return (uint64_t)this;
  }
};

thread_local long found_read_ops = 0;
thread_local long not_found_read_ops = 0;
thread_local long found_write_ops = 0;
thread_local long not_found_write_ops = 0;
thread_local long read_cs_time = 0;
thread_local long write_cs_time = 0;

std::atomic<long> total_found_read_ops = 0;
std::atomic<long> total_not_found_read_ops = 0;
std::atomic<long> total_found_write_ops = 0;
std::atomic<long> total_not_found_write_ops = 0;
std::atomic<long> total_read_cs_time = 0;
std::atomic<long> total_write_cs_time = 0;

void test_hash_table()
{
  auto t1 = high_resolution_clock::now();

  std::shared_ptr<std::vector<cown_ptr<Bucket>>> buckets =
    std::make_shared<std::vector<cown_ptr<Bucket>>>();

  for (size_t i = 0; i < num_buckets; i++)
  {
    std::vector<Entry*> list;
    for (size_t j = 0; j < (num_entries_per_bucket); j++)
      list.push_back(new Entry((num_buckets * j) + i));
    buckets->push_back(make_cown<Bucket>(list));
  }

  for (size_t i = 0; i < num_operations; i++)
  {
    size_t key = rand() % (num_buckets * num_entries_per_bucket * 2);
    size_t idx = key % num_buckets;
    if (rand() % 100 < rw_ratio)
    {
      when(read((*buckets)[idx])) << [key](acquired_cown<const Bucket> bucket) {
        auto t1 = high_resolution_clock::now();

        bool found = false;
        for (auto it : bucket->list)
        {
          if (it->val == key)
          {
            found = true;
            break;
          }
        }

        for (volatile int i = 0; i < read_loop_count; i++)
          Aal::pause();

        if (found)
          found_read_ops++;
        else
          not_found_read_ops++;

        auto t2 = high_resolution_clock::now();

        read_cs_time += duration_cast<nanoseconds>(t2 - t1).count();
      };
    }
    else
    {
      when((*buckets)[idx]) << [key](acquired_cown<Bucket> bucket) {
        auto t1 = high_resolution_clock::now();

        bool found = false;
        for (auto it : bucket->list)
        {
          if (it->val == key)
          {
            found = true;
            break;
          }
        }

        for (volatile int i = 0; i < write_loop_count; i++)
          Aal::pause();

        if (found)
          found_write_ops++;
        else
          not_found_write_ops++;

        auto t2 = high_resolution_clock::now();

        write_cs_time += duration_cast<nanoseconds>(t2 - t1).count();
      };
    }
  }
  auto t2 = high_resolution_clock::now();
  auto ns_int = duration_cast<nanoseconds>(t2 - t1);
  auto us_int = duration_cast<microseconds>(t2 - t1);
  auto ms_int = duration_cast<milliseconds>(t2 - t1);
  std::cout << "Behaviour generation Elapsed time: " << ms_int.count() << "ms "
            << us_int.count() << "us " << ns_int.count() << "ns" << std::endl;
  // No behaviour is executed till now
  assert(
    (total_found_read_ops.load() + total_found_write_ops.load() +
     total_not_found_read_ops.load() + total_not_found_write_ops.load()) == 0);
}

void finish(void)
{
  std::stringstream ss;
  ss << "Thread: " << std::this_thread::get_id()
     << " found read ops: " << found_read_ops
     << " not found read ops: " << not_found_read_ops
     << " found write ops: " << found_write_ops
     << " not found write ops: " << not_found_write_ops << "\n";
  total_found_read_ops.fetch_add(found_read_ops);
  total_not_found_read_ops.fetch_add(not_found_read_ops);
  total_found_write_ops.fetch_add(found_write_ops);
  total_not_found_write_ops.fetch_add(not_found_write_ops);

  total_read_cs_time.fetch_add(read_cs_time);
  total_write_cs_time.fetch_add(write_cs_time);
  std::cout << ss.str();
}

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);

  num_buckets = opt.is<size_t>("--num_buckets", 1);
  num_entries_per_bucket = opt.is<size_t>("--num_entries_per_bucket", 128);
  num_operations = opt.is<size_t>("--num_operations", 100'000'000);
  rw_ratio = opt.is<size_t>("--rw_ratio", 90);
  read_loop_count = opt.is<size_t>("--read_loop_count", 100);
  write_loop_count = opt.is<size_t>("--write_loop_count", 100);

  SystematicTestHarness harness(argc, argv);

  harness.run_at_thread_termination = finish;

  auto t1 = high_resolution_clock::now();
  harness.run(test_hash_table);
  auto t2 = high_resolution_clock::now();

  std::cout << "Num Buckets: " << num_buckets
            << " Num entries per bucket: " << num_entries_per_bucket
            << " Num operations: " << num_operations
            << " Read write ratio: " << rw_ratio
            << " Read loop count: " << read_loop_count
            << " Write loop count: " << write_loop_count << std::endl;

  auto total_ops =
    (total_found_read_ops.load() + total_found_write_ops.load() +
     total_not_found_read_ops.load() + total_not_found_write_ops.load());

  assert(total_ops == num_operations);

  std::cout << "Total ops: " << total_ops
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
