// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../debug/systematic.h"
#include "../ds/wrapindex.h"
#include "debug/logging.h"
#include "work.h"

#include <array>
#include <atomic>
#include <cassert>
#include <utility>

namespace verona::rt
{
  /**
   * Steal-probe ordering selector for WorkStealingQueue::probe_victim.
   *
   *   Heavy = I, E, D   (used by the blocking steal() loop)
   *   Light = I, D, E   (used by per-D-empty fairness probes)
   *
   * Both orders probe I first (lowest cost: relaxed pre-check), then
   * differ only in the tie-breaker for victims with both E and D
   * non-empty: Heavy picks E (typically larger chain, more work per
   * steal); Light picks D (typically smaller already-striped fragments,
   * less work transferred per probe).
   */
  enum class ProbeOrder : uint8_t
  {
    Heavy,
    Light,
  };

  /**
   * Per-core scheduler queue.  Three concurrent stacks per core:
   *
   *   D[N] -- dequeue stack:  owner LIFO push (refill) + pop, stealer
   *                           pop-all (exchange to nullptr only).
   *   E    -- enqueue stack:  owner push (FIFO source), stealer pop-all
   *                           (exchange to nullptr only).
   *   I    -- inbox stack:    any-thread push, owner-drain on D-empty
   *                           refill OR stealer pop-all.  No pop_one
   *                           consumer ever.
   *
   * Operation table:
   *   enqueue(w)         owner:    E.push_one.
   *   enqueue_front(w)   any:      I.push_one  (used by schedule_lifo).
   *   pop_local()        owner:    D[i].pop_one (RR); returns nullptr
   *                                on all-D-empty WITHOUT touching E
   *                                or I.  Cheap hot-path probe.
   *   refill_steal<O>(v) owner:    one fused operation that
   *                                  1. probes one victim's I/E/D
   *                                     (priority order selected by
   *                                     ProbeOrder O = Heavy or Light;
   *                                     skipped on self-probe &v==this),
   *                                  2. pop_alls own E and own I,
   *                                  3. merges into per-bucket stripes
   *                                     (stolen > own_I > own_E),
   *                                  4. publishes into D via a single
   *                                     release fence + N relaxed stores,
   *                                  5. returns the dispatch-priority
   *                                     head, or nullptr if all three
   *                                     sources were empty.
   *   is_empty()         any:      relaxed loads of all N+2 heads.
   *
   * Why pop_local() does not touch I:  pop_local is the hot-path
   * dequeue — it only probes D so the fast path is a single round-
   * robin with no extra branches.  This also ensures that a steady
   * stream of inbox pushes cannot prevent D from emptying, which
   * would starve the fairness token (F1).  A side benefit is that
   * the foreign-write-hot I cacheline is only pulled into the
   * owner's cache during refill_steal, not on every dequeue.
   * When D empties, refill_steal drains I and stripes it on top
   * of E in each D bucket, so the very next pop_local returns
   * inbox items before the owner's own newly-enqueued work.
   *
   * Why a single E (not N E):  Distribution of work across cores happens
   * at drain-into-D time (refill_steal), not at push time.  A single E
   * gives stealers an O(1) probe, and keeps drain-into-D as one walk
   * over one long chain.  K concurrent stealers serialise on E's
   * pop_all RMW, but the winner immediately stripes the chain across
   * its own D[0..N) so the losers pivot to D-steals in one hop --
   * contention is dissipated, not lost.
   *
   * Design rules:
   *   D1. Only the owner publishes non-null chains onto D[k] (via
   *       stripe_into_dequeue, inside refill_steal).  Stealers only
   *       exchange D[k].head to nullptr (pop_all).  Owner pop_one
   *       exchanges D[k].head to nullptr then restores the tail;
   *       stealer pop_all is the only contended head update.
   *   D2. Between pop_local() returning nullptr (i.e. all D[k] observed
   *       null by the owner) and the next refill_steal() returning, no
   *       other code path mutates D[k].head from null to non-null.
   *       Holds because only the owner publishes (D1), only the owner
   *       runs pop_local/refill_steal, and the owner is single-threaded
   *       between them.
   *   E1. Only the owner pushes onto E.  Stealers only exchange
   *       E.head to nullptr (pop_all).
   *   I1. I has no pop_one consumer; ABA is vacuous because the head
   *       can only return to non-null via a fresh push of a Work
   *       allocated by Closure::make (next_in_queue is value-
   *       initialised to nullptr).
   *   F1. (Fairness-drive)  Every owner transition (D non-empty -> D
   *       all-null as observed by pop_local) is followed by exactly
   *       one refill_steal() before the owner runs further user work.
   *       Enforced at the SchedulerThread call site.
   *   F2. (Fairness-spread)  Each refill_steal probes exactly one
   *       victim (caller rotates `victim` once per call), and within
   *       a single probe the victim's D[k] rotation advances the
   *       steal_index rotor.
   */
  template<size_t N>
  class alignas(64) WorkStealingQueue
  {
    // Line A: I -- cross-core write hot.  Alone on its cacheline so
    // foreign-thread push_one CASes do not invalidate owner state.
    alignas(64) std::atomic<Work*> inbox_head{nullptr};

    // Line B: D[N] -- owner LIFO + stealer pop-all.
    alignas(64) std::atomic<Work*> dequeue_heads[N]{};

    // Line C: E -- owner push + stealer pop-all.  Single slot: all
    // fairness striping happens at drain-into-D time, not on push.
    alignas(64) std::atomic<Work*> enqueue_head{nullptr};

    // Line D: owner metadata -- only the owner writes any of these.
    alignas(64) WrapIndex<N> dequeue_index{N - 1};
    WrapIndex<N> steal_index{};

    /**
     * CAS-loop push of a single work item onto a concurrent stack.
     * Release on success pairs with pop_all's acquire exchange.
     */
    static void push(std::atomic<Work*>& head, Work* w)
    {
      Work* old = head.load(std::memory_order_relaxed);
      while (true)
      {
        Systematic::yield();
        w->next_in_queue = old;
        Systematic::yield();
        if (head.compare_exchange_weak(
              old, w, std::memory_order_release, std::memory_order_relaxed))
          return;
      }
    }

    /**
     * Pop the entire chain off `head`.  Returns nullptr if empty.
     * Acquire exchange pairs with push's release CAS.
     */
    template<std::memory_order Order = std::memory_order_acquire>
    static Work* pop_all(std::atomic<Work*>& head)
    {
      Systematic::yield();
      if (head.load(std::memory_order_relaxed) == nullptr)
        return nullptr;
      Systematic::yield();
      return head.exchange(nullptr, Order);
    }

    /**
     * Owner-only publish of a chain onto a head.  Relaxed store;
     * callers provide ordering via a preceding fence or sequencing.
     */
    static void owner_push(std::atomic<Work*>& head, Work* chain)
    {
      head.store(chain, std::memory_order_relaxed);
      Systematic::yield();
    }

    /**
     * Split the head node off a non-null bucket, in place.  Returns
     * the detached node; `bucket` is advanced to the remainder
     * (nullptr if the input was a singleton).
     */
    static Work* split_head(Work*& bucket)
    {
      assert(bucket != nullptr);
      return std::exchange(bucket, bucket->next_in_queue);
    }

    /**
     * Owner pop_one from D.  Returns nullptr if empty.
     *
     * A concurrent stealer can pop_all the chain and free the head
     * before we read head->next, so we exchange-take the whole chain,
     * read next safely, and restore the tail with a plain store.
     * Sound because only the owner publishes non-null to D (D1).
     *
     * All operations are relaxed: HB is established by
     * stripe_into_dequeue's release fence, which a stealer's future
     * acquire pop_all pairs with.
     */
    SNMALLOC_FAST_PATH
    static Work* owner_pop_one(std::atomic<Work*>& head)
    {
      Work* chain = pop_all<std::memory_order_relaxed>(head);
      if (chain == nullptr)
        return nullptr;
      Work* result = split_head(chain);
      owner_push(head, chain);
      return result;
    }

    /**
     * Stripe a non-null `chain` round-robin across `buckets`,
     * prepending each node onto its target bucket.
     *
     * Callers stack sources LOWEST-priority FIRST: each call
     * prepends, so the LAST call's nodes sit at the front of
     * each bucket.
     */
    static void stripe_chain_onto(
      Work* chain, std::array<Work*, N>& buckets, WrapIndex<N>& k)
    {
      assert(chain != nullptr);
      while (chain != nullptr)
      {
        Work* top = split_head(chain);
        top->next_in_queue = std::exchange(buckets[k], top);
        ++k;
      }
    }

    /**
     * Publish buckets into D slots positionally.  D2 guarantees all
     * D[k] are null.  One release fence covers all N relaxed stores.
     */
    SNMALLOC_FAST_PATH void stripe_into_dequeue(std::array<Work*, N>& buckets)
    {
      std::atomic_thread_fence(std::memory_order_release);
      Systematic::yield();
      for (size_t i = 0; i < N; ++i)
      {
        if (buckets[i] == nullptr)
          continue;
        owner_push(dequeue_heads[i], buckets[i]);
      }
    }

    /**
     * Probe a victim queue for work.  Tries I first (cheapest),
     * then E and D in the order selected by `Order` (see ProbeOrder).
     * Returns the first non-empty chain, or nullptr.
     */
    template<ProbeOrder Order>
    SNMALLOC_FAST_PATH Work* probe_victim(WorkStealingQueue& victim)
    {
      if (Work* c = pop_all(victim.inbox_head))
        return c;
      if constexpr (Order == ProbeOrder::Heavy)
      {
        if (Work* c = pop_all(victim.enqueue_head))
          return c;
        for (size_t i = 0; i < N; ++i)
          if (Work* c = pop_all(victim.dequeue_heads[++steal_index]))
            return c;
      }
      else
      {
        for (size_t i = 0; i < N; ++i)
          if (Work* c = pop_all(victim.dequeue_heads[++steal_index]))
            return c;
        if (Work* c = pop_all(victim.enqueue_head))
          return c;
      }
      return nullptr;
    }

  public:
    constexpr WorkStealingQueue() = default;

    ~WorkStealingQueue()
    {
      assert(is_empty());
    }

    /**
     * Owner enqueue onto E.
     */
    void enqueue(Work* w)
    {
      Logging::cout() << "WSQ enqueue " << w << Logging::endl;
      assert(w != nullptr);
      push(enqueue_head, w);
    }

    /**
     * Any-thread push onto the inbox (I).  Caller is responsible
     * for the subsequent Scheduler::get().unpause().
     */
    void enqueue_front(Work* w)
    {
      Logging::cout() << "WSQ inbox push " << w << Logging::endl;
      assert(w != nullptr);
      push(inbox_head, w);
    }

    /**
     * Owner dequeue from D (round-robin).  Returns nullptr when all
     * D slots are empty.  Does NOT probe E or I — caller must call
     * refill_steal on null return (see class header and F1).
     */
    SNMALLOC_FAST_PATH
    Work* pop_local()
    {
      for (size_t attempt = 0; attempt < N; ++attempt)
      {
        Work* w = owner_pop_one(dequeue_heads[dequeue_index--]);
        if (w != nullptr)
          return w;
      }

      return nullptr;
    }

    /**
     * Fused refill + steal.  Called by the owner after pop_local()
     * returns nullptr.
     *
     * Preconditions:
     *   - Every D[k] is empty (D2 holds across the call boundary).
     *   - Called only from the owner SchedulerThread.
     *
     * Self-probe (&victim == this) skips the foreign probe and just
     * drains own E + I, useful inside the blocking steal() spin.
     *
     * Publish/pause race: between pop_alls and stripe_into_dequeue,
     * all heads are null.  A third thread calling check_for_work()
     * during this window may pause.  Caller must unpause after a
     * non-null return so the release stores are visible.
     */
    template<ProbeOrder Order = ProbeOrder::Light>
    SNMALLOC_FAST_PATH Work* refill_steal(WorkStealingQueue& victim)
    {
      // Foreign probe (skipped on self-probe).  On foreign-probe,
      // probe_victim rotates steal_index internally as it walks
      // victim's D[k].
      Work* stolen = (&victim == this) ? nullptr : probe_victim<Order>(victim);

      // Drain own E and own I.
      Work* echain = pop_all(enqueue_head);
      Work* ichain = pop_all(inbox_head);

      if (stolen == nullptr && echain == nullptr && ichain == nullptr)
        return nullptr;

        // D2 precondition: caller just saw pop_local() return nullptr.
#ifndef NDEBUG
      for (size_t k = 0; k < N; ++k)
        assert(dequeue_heads[k].load(std::memory_order_relaxed) == nullptr);
#endif

      // Stripe lowest-priority first (prepend semantics).
      // Final per-bucket layout:
      //   buckets[k] = [I stripe k] -> [stolen stripe k] -> [E stripe k]
      std::array<Work*, N> buckets{};
      WrapIndex<N> k{};

      if (echain != nullptr)
        stripe_chain_onto(echain, buckets, k);
      if (stolen != nullptr)
        stripe_chain_onto(stolen, buckets, k);
      if (ichain != nullptr)
        stripe_chain_onto(ichain, buckets, k);

      k--;
      assert(buckets[k] != nullptr);
      Work* head = split_head(buckets[k]);
      stripe_into_dequeue(buckets);
      dequeue_index = k;
      return head;
    }

    /**
     * Approximate emptiness check.  Relaxed loads of all N+2 heads;
     * may spuriously report false (rare; means we caught a head
     * transitioning).  Cheap enough to call from the pause/unpause
     * termination probe.
     */
    bool is_empty()
    {
      Systematic::yield();
      if (inbox_head.load(std::memory_order_relaxed) != nullptr)
        return false;
      if (enqueue_head.load(std::memory_order_relaxed) != nullptr)
        return false;
      for (size_t i = 0; i < N; ++i)
        if (dequeue_heads[i].load(std::memory_order_relaxed) != nullptr)
          return false;
      return true;
    }
  };
} // namespace verona::rt
