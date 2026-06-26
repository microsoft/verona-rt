/* Verona-rt WSQ backend for PASL work stealing.
 *
 * Implements threadset_private using Verona's 3-stack WSQ with
 * steal-all semantics (pop_all + stripe into D[N]).
 */

#ifndef _VERONA_WSQ_PASL_H_
#define _VERONA_WSQ_PASL_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>
#include <cassert>
#include <vector>

#include "workstealing.hpp"

namespace pasl {
namespace sched {
namespace workstealing {

namespace verona_impl {

struct WorkItem {
  WorkItem* next_in_queue{nullptr};
  thread_p payload{nullptr};
};

template<size_t N>
class WrapIndex {
  size_t idx_;
public:
  constexpr WrapIndex(size_t i = 0) : idx_(i % N) {}
  operator size_t() const { return idx_; }
  WrapIndex& operator++() { idx_ = (idx_ + 1) % N; return *this; }
  WrapIndex operator++(int) { auto old = *this; ++*this; return old; }
  WrapIndex& operator--() { idx_ = (idx_ + N - 1) % N; return *this; }
  WrapIndex operator--(int) { auto old = *this; --*this; return old; }
};

template<size_t N = 4>
class alignas(64) WSQ {
  alignas(64) std::atomic<WorkItem*> inbox_head{nullptr};
  alignas(64) std::atomic<WorkItem*> dequeue_heads[N]{};
  alignas(64) std::atomic<WorkItem*> enqueue_head{nullptr};
  alignas(64) WrapIndex<N> dequeue_index{N - 1};
  WrapIndex<N> steal_index{};

  static void push(std::atomic<WorkItem*>& head, WorkItem* w) {
    WorkItem* old = head.load(std::memory_order_relaxed);
    while (true) {
      w->next_in_queue = old;
      if (head.compare_exchange_weak(old, w,
            std::memory_order_release, std::memory_order_relaxed))
        return;
    }
  }

  template<std::memory_order Order = std::memory_order_acquire>
  static WorkItem* pop_all(std::atomic<WorkItem*>& head) {
    if (head.load(std::memory_order_relaxed) == nullptr)
      return nullptr;
    return head.exchange(nullptr, Order);
  }

  static void owner_push(std::atomic<WorkItem*>& head, WorkItem* chain) {
    head.store(chain, std::memory_order_relaxed);
  }

  static WorkItem* split_head(WorkItem*& bucket) {
    return std::exchange(bucket, bucket->next_in_queue);
  }

  static WorkItem* owner_pop_one(std::atomic<WorkItem*>& head) {
    WorkItem* chain = pop_all<std::memory_order_relaxed>(head);
    if (chain == nullptr) return nullptr;
    WorkItem* result = split_head(chain);
    owner_push(head, chain);
    return result;
  }

  static void stripe_chain_onto(
      WorkItem* chain, std::array<WorkItem*, N>& buckets, WrapIndex<N>& k) {
    while (chain != nullptr) {
      WorkItem* top = split_head(chain);
      top->next_in_queue = std::exchange(buckets[k], top);
      ++k;
    }
  }

  void stripe_into_dequeue(std::array<WorkItem*, N>& buckets) {
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < N; ++i) {
      if (buckets[i] == nullptr) continue;
      owner_push(dequeue_heads[i], buckets[i]);
    }
  }

  WorkItem* probe_victim(WSQ& victim) {
    if (WorkItem* c = pop_all(victim.inbox_head)) return c;
    if (WorkItem* c = pop_all(victim.enqueue_head)) return c;
    for (size_t i = 0; i < N; ++i)
      if (WorkItem* c = pop_all(victim.dequeue_heads[++steal_index]))
        return c;
    return nullptr;
  }

public:
  constexpr WSQ() = default;

  void enqueue(WorkItem* w) { push(enqueue_head, w); }
  void enqueue_front(WorkItem* w) { push(inbox_head, w); }

  WorkItem* pop_local() {
    for (size_t attempt = 0; attempt < N; ++attempt) {
      WorkItem* w = owner_pop_one(dequeue_heads[dequeue_index--]);
      if (w != nullptr) return w;
    }
    return nullptr;
  }

  WorkItem* self_drain() {
    WorkItem* echain = pop_all(enqueue_head);
    WorkItem* ichain = pop_all(inbox_head);
    if (echain == nullptr && ichain == nullptr) return nullptr;
    std::array<WorkItem*, N> buckets{};
    WrapIndex<N> k{};
    if (echain != nullptr) stripe_chain_onto(echain, buckets, k);
    if (ichain != nullptr) stripe_chain_onto(ichain, buckets, k);
    k--;
    WorkItem* head = split_head(buckets[k]);
    stripe_into_dequeue(buckets);
    dequeue_index = k;
    return head;
  }

  WorkItem* refill_steal(WSQ& victim) {
    WorkItem* stolen = (&victim == this) ? nullptr : probe_victim(victim);
    WorkItem* echain = pop_all(enqueue_head);
    WorkItem* ichain = pop_all(inbox_head);
    if (stolen == nullptr && echain == nullptr && ichain == nullptr)
      return nullptr;
    std::array<WorkItem*, N> buckets{};
    WrapIndex<N> k{};
    if (echain != nullptr) stripe_chain_onto(echain, buckets, k);
    if (stolen != nullptr) stripe_chain_onto(stolen, buckets, k);
    if (ichain != nullptr) stripe_chain_onto(ichain, buckets, k);
    k--;
    WorkItem* head = split_head(buckets[k]);
    stripe_into_dequeue(buckets);
    dequeue_index = k;
    return head;
  }

  bool is_empty() {
    if (inbox_head.load(std::memory_order_relaxed) != nullptr) return false;
    if (enqueue_head.load(std::memory_order_relaxed) != nullptr) return false;
    for (size_t i = 0; i < N; ++i)
      if (dequeue_heads[i].load(std::memory_order_relaxed) != nullptr)
        return false;
    return true;
  }
};

} // namespace verona_impl

/*---------------------------------------------------------------------*/

class verona_wsq_shared : public threadset_shared {
public:
  data::perworker::array<verona_impl::WSQ<4>*> queues;

  verona_wsq_shared() {
    for (worker_id_t id = 0; id < util::worker::get_nb(); id++)
      queues[id] = new verona_impl::WSQ<4>();
  }
  ~verona_wsq_shared() {
    for (worker_id_t id = 0; id < util::worker::get_nb(); id++)
      delete queues[id];
  }
  friend class verona_wsq_private;
};

class verona_wsq_private : public threadset_private {
protected:
  verona_wsq_shared* shared;
  verona_impl::WSQ<4>* my_queue;

  bool stay_in_acquire() {
    return scheduler::_private::stay() && !local_has();
  }

  virtual void add_to_pool_of_ready_threads(thread_p t) {
    local_push(t);
  }

public:
  verona_wsq_private(verona_wsq_shared* s) : shared(s) {}

  void init() {
    allow_interrupt = false;
    scheduler::_private::init();
    my_queue = shared->queues[my_id];
  }

  void destroy() { scheduler::_private::destroy(); }

  size_t nb_threads() { return 0; }
  bool local_has() { return !my_queue->is_empty(); }

  void local_push(thread_p thread) {
    auto* w = new verona_impl::WorkItem();
    w->payload = thread;
    my_queue->enqueue(w);
  }

  thread_p local_pop() {
    auto* w = my_queue->pop_local();
    if (!w) w = my_queue->self_drain();
    if (w) {
      thread_p t = w->payload;
      return t;
    }
    return nullptr;
  }

  thread_p local_peek() { return nullptr; }
  bool remote_has() { return !my_queue->is_empty(); }
  void remote_push(thread_p thread) { local_push(thread); }
  thread_p remote_pop() { return nullptr; }

  void run() {
    while (stay()) {
      thread_p t = local_pop();
      if (t != nullptr) {
        exec(t);
        check();
      } else {
        wait();
      }
    }
  }

  void acquire() {
    if (nb_workers < 2) return;
    int attempts = 0;
    while (stay_in_acquire()) {
      worker_id_t victim_id = random_other();
      auto* victim_q = shared->queues[victim_id];
      auto* w = my_queue->refill_steal(*victim_q);
      if (w) {
        thread_p t = w->payload;
        STAT_COUNT(THREAD_SEND);
        add_to_pool_of_ready_threads(t);
        return;
      }
      attempts++;
      if (attempts > nb_workers * 2) {
        attempts = 0;
        util::worker::controller_t::yield();
      }
    }
  }

  void communicate() {}
  void wait() { enter_wait(); acquire(); exit_wait(); }
  void check() { scheduler::_private::check_periodic(); }
  void check_on_interrupt() {}
};

} // namespace workstealing
} // namespace sched
} // namespace pasl

#endif
