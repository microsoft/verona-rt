# Plan: Verona WSQ as Cilk Runtime Backend

## Problem

We want to answer: **does Verona's work-stealing topology outperform Cilk's THE deque for fork/join workloads?**

Previous attempts to answer this were confounded by:
- Allocation differences (coroutine frames, PASL ucontext stacks)
- Execution model differences (inline vs scheduled)
- Synthetic benchmarks that don't represent real fork/join code

Plugging Verona's WSQ into OpenCilk's cheetah runtime eliminates all confounds:
same compiler, same cactus stacks, same programs — only the deque changes.

## Background: Current Cilk THE Protocol

Each worker has `head`, `tail`, `exc` pointers into a fixed array (the LTQ):
- **Push (detach):** `*tail++ = parent` — plain store + release on tail
- **Pop (child return):** `--tail` + seq_cst store, then Dekker check (`exc > tail` → stolen)
- **Steal:** CAS on `head` — one frame at a time

The Dekker protocol avoids locks but requires seq_cst on every pop.

## Design: Verona WSQ Replacement

### Core Idea

Replace the LTQ array + THE protocol with Verona's D[N] slots only.
E and I are unused for this experiment — fork/join is depth-first,
so we only need the LIFO stack behaviour that D provides.

Use the join counter with self-count (Verona pattern) instead of the
Dekker protocol for steal detection.

### D-only WSQ as a LIFO Stack

D[N] (N=4) acts as a partitioned LIFO stack:
- Owner pushes to `D[dequeue_index]` via CAS, then decrements index
- Owner pops from `D[dequeue_index]` via relaxed exchange, increments index
- Thieves steal from any `D[k]` via `pop_all(D[k])` exchange

This gives LIFO ordering for the owner (depth-first) while items
are immediately stealable. A thief that exchanges `D[k]` gets the
entire chain in that bucket (~1/4 of total work).

No E, no I, no refill_steal merging — just D slots as concurrent
stacks. This is the simplest possible starting point. Profile first,
optimise later.

### Join Counter with Self-Count

Cilk currently uses `Closure::join_counter` + `Closure::status` + a
mutex to coordinate sync. Replace with Verona's self-count pattern:

```
enter_frame:    join_counter = 1        (self-count)
detach(child):  join_counter++          (child spawned)
child_done:     join_counter--          (atomic, check for zero)
cilk_sync:      join_counter--          (remove self-count)
                if != 0 → suspend      (children outstanding)
                if == 0 → continue      (all done)
```

Whoever decrements to zero wins — no mutex, no status field needed.

### New Operation: `push_lifo` on D

Add to the WSQ:
```cpp
void push_lifo(node_t w) {
    auto& slot = dequeue_heads[dequeue_index--];
    node_t old = slot.load(relaxed);
    while (true) {
        w->wsq_next = old;
        if (slot.compare_exchange_weak(old, w, release, relaxed))
            return;
    }
}
```

CAS because a thief can `pop_all(D[k])` concurrently. Uncontended
CAS is ~15-30ns on x86 — same order as Cilk's store + release.

### Push Path: `__cilkrts_detach`

```cpp
void __cilkrts_detach(sf, parent) {
    w->join_counter++;
    w->wsq.push_lifo(parent);   // CAS push to D[dequeue_index--]
}
```

### Pop Path: `__cilkrts_leave_frame_helper`

```cpp
void __cilkrts_leave_frame_helper(sf, parent) {
    sf->flags &= ~CILK_FRAME_DETACHED;
    node_t popped = w->wsq.pop_local();  // relaxed exchange from D
    if (popped == parent)
        return;  // not stolen, continue into parent
    // Parent was stolen (popped is null or a different frame).
    // Push back any wrong item.
    if (popped) w->wsq.push_lifo(popped);
    // This worker's parent is gone — jump to scheduler.
    // Parent will be rescheduled when join_counter hits zero.
    longjmp_to_runtime(w);
}
```

Note: pop_local may return a different frame if items were reordered
across D[4]. In practice with depth-first execution and low fan-out,
the most recent push is in the current D slot and comes back first.
If this proves problematic, we can track the D index at push time.

### Sync Path: `__cilk_sync`

```cpp
void __cilk_sync(sf) {
    if (sf->join_counter.fetch_sub(1, acq_rel) == 1) {
        // Was 1, now 0 — all children already done. No-op.
        return;
    }
    // Children outstanding — suspend
    setjmp(sf->ctx);
    longjmp_to_runtime(w);
    // Resumed here when last child decrements to zero
}
```

### Steal Path

When a worker is idle:

```cpp
// Try each victim's D slots directly
for each victim {
    for k in 0..N {
        chain = pop_all(victim->wsq.D[k]);
        if (chain) {
            // Got work. Stripe into own D slots.
            stripe_into_own_D(chain);
            item = pop_local();
            // Process item, rest available via pop_local
            do_what_it_says_boss(w, frame_to_closure(item));
            return;
        }
    }
}
```

A single `pop_all(D[k])` exchange grabs the entire chain in one
bucket. The thief stripes it across its own D[4] and processes
items via `pop_local`. No repeated CAS attempts.

### Stack Frame Change

```c
struct __cilkrts_stack_frame {
    // ... existing fields ...
    __cilkrts_stack_frame *wsq_next;  // intrusive D-chain linkage
};
```

### Worker Change

```c
struct __cilkrts_worker {
    // Remove: tail, head, exc, ltq_limit
    // Add:
    alignas(64) std::atomic<__cilkrts_stack_frame*> D[4];
    WrapIndex<4> dequeue_index{3};
};
```

## Implementation Steps

1. Fork cheetah, add `wsq_next` to `__cilkrts_stack_frame`
2. Replace THE fields (head/tail/exc) with D[4] + dequeue_index
3. Implement `push_lifo` and `pop_local` on D
4. Implement new `__cilkrts_detach` using `push_lifo`
5. Implement new `__cilkrts_leave_frame_helper` using `pop_local`
6. Replace join_counter/status/mutex with self-count atomic protocol
7. Implement new `__cilk_sync` using self-count
8. Replace steal loop with D-slot `pop_all` + stripe
9. Keep all fiber/cactus-stack management unchanged
10. Build with OpenCilk compiler, run cilkbench suite
11. Compare against unmodified cheetah on fib, nqueens, sort, hull, SpMV
12. Profile to identify actual bottlenecks before optimising

## What This Measures

- D-only steal-all vs Cilk's THE steal-one on real fork/join programs
- Same compiler, same cactus stacks, same benchmarks
- Local-path cost: CAS push + relaxed pop (Verona D) vs
  store + seq_cst Dekker (Cilk THE)
- Steal efficiency: one exchange grabs a chain vs one CAS per frame
- Profiling data tells us where the time actually goes

## Risks

- OpenCilk compiler must be built from source (~30-60 min, needs 15GB disk)
- Cheetah's steal path is entangled with closure/fiber management —
  extracting the deque cleanly requires careful surgery
- The `exc` pointer serves double duty (steal detection + exception
  handling) — exception support may need rethinking
- `pop_local` may return a wrong frame under D[4] reordering — need
  to verify this doesn't break correctness for fork/join patterns
- CAS on push may be more expensive than Cilk's plain store in
  practice — the profiling will tell us
