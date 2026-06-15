# Read-only cowns

This directory contains a C# model of Behaviour-Oriented Concurrency
**with the read-only cown extension**, together with the protocol
explainer below.

The original BoC paper ([OOPSLA '23][bocpaper]) gives each cown
**exclusive** access: at most one behaviour holds a given cown at a time.
The read-only extension lets a behaviour declare it only *reads* a cown,
and many read-mode behaviours on the same cown may run in parallel. The
protocol is careful to preserve all the original guarantees (deadlock
freedom, atomic multi-cown acquisition, FIFO fairness for writers).

The C# model is a deliberate *port* of the production C++ implementation in
[`src/rt/boc/behaviourcore.h`](../../../../src/rt/boc/behaviourcore.h) and
[`src/rt/boc/cown.h`](../../../../src/rt/boc/cown.h) with three things
left out: the orthogonal *atomic multi-schedule* feature, plus two
production concerns (intrusive ref-counting and the `n==1` direct-dispatch
shortcut). The read-only protocol logic is identical; see
["What we left out"](#11-what-we-left-out) at the end.

For the simpler exclusive-access model (the one in the paper), see the
sibling [`../modelimpl/`](../modelimpl) directory — that model is the
recommended starting point; everything below builds on top of it.

### Build and run

Requires .NET 7 SDK (on Ubuntu: `apt install dotnet-sdk-7.0`).

```sh
dotnet build && dotnet run
```

[`Program.cs`](Program.cs) exercises 18 scenarios: the original
write-only set plus solo read, W→R→R→W cascade, two concurrent reads,
duplicate cowns (`when(c1, c1)`, `when(c2, read(c2))`,
`when(read(c1), read(c1))`), and mixed read/write.

[bocpaper]: https://www.microsoft.com/en-us/research/wp-content/uploads/2023/10/oopslab23main-p418-p-b0310c7662-70953-final.pdf

---

## 1. What you can write

```csharp
var c1 = new Cown<int>(1);
var c2 = new Cown<int>(2);

when(c1)(x => ...);                       // write c1
when(read(c1))(x => ...);                 // read c1
when(c1, read(c2))((x, y) => ...);        // write c1, read c2
when(read(c1), read(c2))((x, y) => ...);  // read both -- may run with another
                                          //   "read both" concurrently
```

**Duplicate cowns are allowed.** All three of the following are well-formed
and execute as if the cown appeared once, choosing the strongest mode
requested:

```csharp
when(c1, c1)((x, y) => ...);                   // de-duplicated to one write
when(c1, read(c1))((x, y) => ...);             // write wins
when(read(c1), read(c1))((x, y) => ...);       // de-duplicated to one read
```

**Semantic contract.** A read-mode body must not mutate the cown's contents.
This model does NOT enforce that — the body signature is `Action<T>` for both
modes (see ["What we left out"](#11-what-we-left-out)). The contract is the
user's responsibility; in production C++ the `Access<const T>` overload
enforces it through the type system.

---

## 2. Per-cown state

Each cown carries three fields (see `CownBase` in `When.cs`, mirroring
`Cown` in `boc/cown.h`):

```
+----------------+
| CownBase       |
+----------------+
| last           |---->  newest Request in the MCS-style queue
|                |       (null if no behaviours are waiting)
| nextWriter     |---->  Behaviour parked behind active readers
|                |       (null if the active holder is not a writer waiting,
|                |        OR if no behaviour is currently active here)
| readRefCount   |       packed int:  [reader_count << 1 | writer_waiting_bit]
+----------------+
```

`last` is the tail of a per-cown queue of `Request`s. New behaviours enqueue
themselves by atomically swapping into `last`; the predecessor (if any) is
the one that has to wake them when it finishes.

`readRefCount` is only consulted when readers are active. The high bits hold
the number of active readers; the low bit is the **writer-waiting bit**, set
by a writer that arrived while readers were still running.

`nextWriter` is the parked writer's `Behaviour` (not `Request`) — see
[§6](#6-last-reader-wakes-writer).

---

## 3. Per-request state

Each `Request` (one queue node, mirroring `Slot` in C++) carries a
`status` field which encodes the queue link. In C++ this is a single
atomic word with low-bit tagging. In the C# model it is a sealed sum type
(value-equality on the runtime type is what makes the encoding portable to
Scala/Rust/Python without bit-tagging tricks).

The six states:

| State              | Meaning                                                                                                |
|--------------------|--------------------------------------------------------------------------------------------------------|
| `Wait`             | 2PL is still in progress for this Request. Successors must spin until it advances.                     |
| `Ready`            | 2PL complete, no successor has linked yet. **The only contended state.**                               |
| `ReadAvailable`    | I am a live reader. New readers may join the chain by closing it on me.                                |
| `ChainClosed`      | A reader successor observed `ReadAvailable` on me; my `Release` will not notify next.                  |
| `NextReader(r)`    | My successor is a reader Request `r`.                                                                  |
| `NextWriter(b)`    | My successor is a writer Behaviour `b` (NB: behaviour, not request — see §6).                          |

```
                         StartEnqueue
                              v
                          +--------+
                          |  Wait  |   (successors spin past this state)
                          +--------+
                              |
                              |  FinishEnqueue (self, plain store)
              +---------------+---------------+
              |                               |
              | non-head reader / writer      | head reader (predecessor
              v                               v  was ReadAvailable)
         +---------+    cascade walker   +-----------------+
         |  Ready  |--(CAS, by writer  ->| ReadAvailable   |
         |         |   releasing chain)  +-----------------+
         +---------+                              |
           |   |                                  | successor reader joins
           |   |                                  | (plain store, after
           |   |                                  v  CAS to Next* failed)
           |   |                           +-------------+
           |   |                           | ChainClosed |
           |   |                           +-------------+
           |   |
           |   +-- successor reader (CAS) --> +---------------+
           |                                  | NextReader(r) |
           |                                  +---------------+
           |
           +------ successor writer (CAS) --> +---------------+
                                              | NextWriter(b) |
                                              +---------------+
```

`Ready` is the only contended state: all three of its outgoing CAS
transitions race against each other and against the cascade walker's
CAS to `ReadAvailable`.

Three actors write to a given slot's `status` after construction:

- **The slot itself** in `FinishEnqueue` (plain store): `Wait → Ready` or
  `Wait → ReadAvailable`.
- **The successor** in `StartEnqueue`, acting on its *predecessor*: CAS
  `Ready → NextReader(self)` / `Ready → NextWriter(self.behaviour)`, or
  (after observing `ReadAvailable`) plain-store `ReadAvailable →
  ChainClosed`.
- **The cascade walker** (a writer's `Release` walking the chain behind
  it): CAS `Ready → ReadAvailable` on each reader it walks past.

These actors share two writable starting states (`Ready` and
`ReadAvailable`); the spin-while-`Wait` in both the successor's
`StartEnqueue` and the cascade walker is what keeps them from racing the
slot's own `FinishEnqueue` publication.

---

## 4. 2PL with read mode

Each `when(...)` creates one `Behaviour` containing one `Request` per cown
listed (after sort + dedup). `Schedule` does the standard two-phase locking
walk, but split into four passes (matching the C++ `schedule_many`):

1. **Phase 1 — sort + dedup**. Sort the request list by `(target, isRead)`
   so that writes (`isRead == false`) sort before reads within the same
   cown. Then walk the list and mark every request whose target equals its
   predecessor's as a *duplicate*. Duplicates are not enqueued and their
   `Release` is a no-op.
2. **Phase 2 — StartEnqueue**: for each non-duplicate request, atomically
   swap `target.last` to point at it, save the predecessor. The status of
   the new request stays `Wait` (set at construction) until phase 3
   publishes it. This is the linearisation point that establishes the
   global acquisition order — crucially, all of a behaviour's
   `StartEnqueue`s happen **before** any of its `FinishEnqueue`s do.
   StartEnqueue ALSO does the predecessor-respond step: it spins until the
   predecessor has finished its own phase 3 (so the predecessor's status
   is not `Wait`), then CASes the predecessor (`Ready → NextReader`,
   `Ready → NextWriter`, or — if the predecessor is `ReadAvailable` —
   plain-stores `ChainClosed` and short-circuits via the read-chain join).
3. **Phase 3 — FinishEnqueue**: publish *our own* status. A self-resolving
   reader (no predecessor, or joined a live read chain) publishes
   `ReadAvailable`; every other slot publishes `Ready`. After this, future
   enqueuers can proceed past us.
4. **Phase 4 — PostEnqueue**: for slots that are self-resolving (the
   `selfResolve` flag in code), arrange for `ResolveOne` to fire. Readers
   can always resolve (their `AddRead` happened in StartEnqueue). Writers
   check the cown's `readRefCount`: if no readers are active, call
   `TryWrite` and `ResolveOne`; else park themselves as `cown.nextWriter`
   and wait for the last reader.
5. **Trailing safety decrement**. `Behaviour.count` starts at
   `(unique requests) + 1`. The trailing decrement runs at the end of
   `Schedule`. This +1 is essential: it guarantees the body cannot fire
   before phases 2/3/4 are all complete, even if every request had no
   predecessor and could resolve immediately.

This is the same protocol as for write-only cowns, with two adjustments:
the sort comparator now uses `(target, isRead)` (writers sort first), and
the predecessor-respond step in phase 2 has the `ReadAvailable` shortcut.

---

## 5. The two contended CASes

Two atomic operations carry the entire contention story:

**(a) The predecessor's response — `TrySetNextReader/Writer`.** A
successor in StartEnqueue walks to its predecessor (after spinning until
the predecessor leaves `Wait`) and CASes that predecessor's `status`
field. There are two cases:
- predecessor was `Ready` (no successor yet): we win; predecessor's
  Release will resolve us.
- predecessor was `ReadAvailable` and we are also a reader: we lose; we
  *close* the chain by plain-storing `ChainClosed` on the predecessor
  (its release path skips us) and join the live read set ourselves.

**(b) The writer-opens-a-chain shortcut — `TrySetReadAvailable`.** When a
writer finishes and its status is `NextReader(R1)`, the writer's Release
walks the chain forward (see §7). For each reader it walks past, it CASes
the reader's status from `Ready` to `ReadAvailable`. Success: this reader
is now the head of a live read group. Failure: the reader's status is
already `NextReader(_)` or `NextWriter(_)` (a successor linked behind it
while we were walking); the walker reads the link and continues.

### Worked race trace: two readers joining concurrently behind a running writer

State: a writer `W` is running on cown `c`. Two reader requests `R1`,
`R2` arrive in quick succession (suppose R1 wins the `last` swap).

```
T0:  c.last = W           W.status = Ready
T1:  R1.StartEnqueue:
       swap c.last = R1, prev = W. R1.status = Wait.
       Spin while W.status is Wait  (W is Ready, so spin exits at once).
       CAS W.status: Ready -> NextReader(R1)              (W now knows about R1)
T2:  R2.StartEnqueue:
       swap c.last = R2, prev = R1. R2.status = Wait.
       Spin while R1.status is Wait  (R1.status is still Wait
       because R1.FinishEnqueue hasn't run yet).
       Eventually R1.FinishEnqueue runs (T3 below); R2's spin
       sees R1.status = Ready and continues.
T3:  R1.FinishEnqueue: R1 had a predecessor (W), so R1 is not
       self-resolving; R1.status := Ready.
T4:  R2 (still in StartEnqueue): now sees R1.status = Ready.
       CAS R1.status: Ready -> NextReader(R2).            (R1 now knows about R2)
T5:  R2.FinishEnqueue: R2.status := Ready.
T6:  W's body finishes; W.Release() sees W.status = NextReader(R1).
       It calls WakeupReaderChain(R1):
         - AddRead(1)                                   (cover R1)
         - Pass 1: walk the chain, counting readers.
             - Spin until R1.status is not Wait (it's NextReader(R2)).
             - TrySetReadAvailable(R1) FAILS (status is NextReader(R2)).
             - readerCount = 1; walk into R2.
             - Spin until R2.status is not Wait (it's Ready).
             - TrySetReadAvailable(R2) succeeds. readerCount = 2. Done.
         - AddRead(readerCount - 1) = AddRead(1) for R2.
         - Pass 2: re-traverse from R1, resolving each behaviour
             (capture R2 link from R1.status before calling ResolveOne).
```

The crucial property: the cascade walker **does not skip past `Wait`**. It
always spins until the request has transitioned out of `Wait`, because the
state encodes *the successor link*, which is the only correct thing to walk.
Treating `Wait` as "writer-successor by elimination" is exactly the
cascade-walker `Wait` race that the spin guards against (and that the C++
runtime fix in
[`behaviourcore.h:159`](../../../../src/rt/boc/behaviourcore.h) addresses).

---

## 6. Last-reader-wakes-writer

When a writer `W` arrives while readers are active on cown `c`, it cannot
be in the `Wait/Ready/NextReader` chain — there is no slot in the chain
to attach behind (the chain head is a live `ReadAvailable` reader). So
`W` is parked at a side-channel, `cown.nextWriter`, and the last reader's
release routes the wakeup through that channel rather than through the
chain.

### Asymmetry: `NextReader` carries a `Request`, `NextWriter` carries a `Behaviour`

The two link variants do not carry the same payload, because the cascade
walker uses them differently:

- When the walker reaches `NextReader(r)`, it must look at `r.status` to
  find what is next behind that reader. So it needs the `Request`.
- When the walker reaches `NextWriter(b)`, it **stops**. The writer is
  parked (not part of the chain); the walker records `b` as the trailing
  writer and never traverses further. So all it needs is the `Behaviour`
  to schedule later.

A port can pick whatever encoding fits its language (sealed enum, tagged
pointer, two distinct atomic-cells, ...), but the distinction between
"reader link → traversable" and "writer link → terminal" should stay
explicit. Carrying a `Request` in `NextWriter` would not be wrong, but it
muddies the role: a reader of the code can no longer tell from the
declaration alone that the walker never inspects fields of a writer
link's payload.

### Protocol

The full handshake — the writer parking itself in `PostEnqueue`, the last
reader detecting `LastReaderWaitingWriter` and routing the wakeup — is
implemented in `When.cs` (`PostEnqueue`, `ReadRefCount.ReleaseRead`, and
`CownBase.WakeupNextWriter`). The crux is the spin in
`WakeupNextWriter`: between a writer setting the writer-waiting bit
(inside `TryWrite`) and the writer storing itself into `cown.nextWriter`
(inside `PostEnqueue`), the last reader can already be releasing. The
reader must spin until `nextWriter` is non-null, otherwise the writer is
lost.

The subtle point ([`cown.h:84`](../../../../src/rt/boc/cown.h)): in the
`LastReaderWaitingWriter` path the reader must reset the count to 0, not
just decrement. Otherwise a subsequent `Add(-2)` on the same counter from a
stale path would underflow.

---

## 7. Writer-wakes-readers cascade

When a writer finishes and its `status` is `NextReader(R1)`, the wakeup is
not "schedule R1 and you're done" — there may be a *chain* of readers
queued behind R1 that have not yet been linked into the read set. The
writer's `Release` walks the chain in **two passes** (see
`WakeupReaderChain` in [`When.cs`](When.cs) for the full code):

- **Pass 1** walks forward following `NextReader` links, CASes a
  terminator slot from `Ready` to `ReadAvailable` (or stops at a
  `NextWriter` link, parking that writer), and counts how many readers
  it walked past. It does **not** resolve any reader in this pass.
- **Pass 2** re-walks the chain via the same (immutable) `NextReader`
  links, calling `ResolveOne` on each reader's behaviour. Each `next`
  link is captured before `ResolveOne` because the slot can become
  garbage the moment its body fires.

Three properties hold the design together:

- **Spin past `Wait`.** `StartEnqueue` swaps `last`, but the slot's own
  status is published in `FinishEnqueue`. Between those two steps the
  slot exists in the chain with `status == Wait`. The walker must not
  interpret `Wait` as "writer-successor by elimination" — that
  mis-classification is the cascade-walker `Wait` race that this PR's
  C++ fix addresses (and that the systematic test
  [`test/func/readonly_cascade`](../../../../test/func/readonly_cascade/readonly_cascade.cc)
  reproduces).
- **Pre-register `AddRead(1)` before publishing `ReadAvailable`.** A
  writer arriving mid-cascade can chain-close on the terminator we just
  published. Without the up-front `AddRead(1)` that writer would see
  `readRefCount == 0`, take `TryWrite`'s fast path, and start executing
  concurrently with the readers we are about to wake.
- **Two passes, not one.** Resolving inside pass 1 would race a reader's
  own `Release` against our continued walk through the chain — the slot
  might become garbage while we still need its successor link. Pass 1
  only mutates `readRefCount` and individual `status` fields; pass 2
  captures each link before resolving.

---

## 8. Worked examples

### 8.1 W → R → R → W on cown `c`

```
when(c)(...) ;        // W1
when(read(c))(...) ;  // R1
when(read(c))(...) ;  // R2
when(c)(...) ;        // W2
```

- W1 arrives on empty cown → status `Ready` → runs body → `Release`:
  status is still `Ready` (no successor yet) ⇒ CAS `c.last = null`. Done.
  *(If R1 had arrived in between, W1's CAS to null would fail and it would
  spin until R1's `FinishEnqueue` publishes `NextReader(R1)`; then it
  cascades.)*
- R1 enqueues. Suppose this is **after** W1's body completes but **before**
  W1's CAS-to-null. W1's CAS-to-null fails, W1 spins; R1's FinishEnqueue
  publishes status NextReader(R1) on W1; W1's Release reads NextReader(R1),
  cascade-walks → `TrySetReadAvailable(R1)`, `AddRead(1)`, `ResolveOne(R1)`.
- R2 enqueues onto R1. R1 is `ReadAvailable`, so R2 CASes its OWN status
  to `ChainClosed` and joins the read set (`AddRead(1)` itself,
  `ResolveOne`). R1 is unaware.
- W2 enqueues. Predecessor is R2 (just joined). R2 is `ChainClosed`, not
  `ReadAvailable`. W2 CASes R2.status from `ChainClosed` to `NextWriter(W2)`.
  W2 sets `cown.nextWriter = W2.behaviour`, calls `TryWrite` → returns
  false (readers active). W2 sleeps.
- R1.Release → `DropRead` → readRefCount goes from 4 to 2 (one reader left).
- R2.Release → `DropRead` → newValue == WRITER_WAITING_BIT (=1) ⇒ clear,
  return `LastReaderWaitingWriter` ⇒ wake `cown.nextWriter` = W2.
- W2 runs.

### 8.2 Two-reader join race

See [§5](#5-the-two-contended-cases) above for the trace.

### 8.3 Duplicate cowns: `when(read(c), read(c))`

Sort + dedup produces a single non-duplicate request `R` (and one
duplicate slot). `Behaviour.count` starts at `2` (one non-duplicate + 1
safety). Schedule enqueues `R`, calls `ResolveOne` once when `R` becomes
ready, and once again at the tail of `Schedule`. Body runs with both
arguments bound to the same cown contents.

### 8.4 Mixed duplicate: `when(c1, c1)` (both writes)

Sort puts both at the same target, writers first, so both are writers; the
second one is marked duplicate. `Behaviour.count = 2`. Same flow as above.

### 8.5 `when(c, read(c))`

Sort: writer comes first (`!false=true > !true=false` in our ordering),
so `(c, write)` precedes `(c, read)`. The read slot is the duplicate. The
behaviour holds `c` in write mode (write wins).

---

## 9. Duplicate cowns

After sorting by `(target, !isRead)`:

1. Within a single cown, the write slot (if any) comes first.
2. Any subsequent slot for the same cown is marked `isDuplicate = true`.
3. `Behaviour.count` is `(unique slots) + 1`.
4. Duplicates' `Release` is a no-op.
5. The non-duplicate slot's `Release` is the one that does the cown work.

This is the same logic as in `boc/behaviourcore.h` (search for
`COWN_DUPLICATE_FLAG`).

---

## 10. Correctness sketch

**Progress.** Every Request that enqueues either (a) is `Ready` and
`ResolveOne` is called on its behaviour immediately, or (b) is woken by
exactly one predecessor's `Release`. The cascade walker visits every
reader in the chain it discovers exactly once.

**No deadlock.** 2PL acquires cowns in a globally consistent order (sorted
by identity). All cowns are acquired before the body runs. The body cannot
acquire new cowns synchronously — `when(...)` only schedules.

**No double-schedule.** `Behaviour.count` decrements to zero exactly once
per behaviour. The +1 safety means phase 2/3 of `Schedule` always observes
`count >= 1` until its own trailing `ResolveOne`.

**Read-after-write atomic visibility.** A reader linked behind a writer
runs only after the writer's `Release`. In the writer-cascade path, the
reader's `ResolveOne` is called by the writer's Release, which is
sequenced after the writer's body via `try/finally`.

---

## 11. What we left out

The C# model leaves out one orthogonal protocol feature (atomic
multi-schedule) and two production concerns (intrusive reference
counting and the `exec_count_down(n==1)` direct-dispatch shortcut). See
[`CPP_MAPPING.md`](CPP_MAPPING.md) for a brief explanation of each.

**Read-only enforcement.** The body of `when(read(c))(...)` receives a `T`,
not a `T` with `const` qualifier. The model does not prevent a user from
mutating it; it relies on the contract. In production C++ this is
type-enforced via `Access<const T>`; doing the same in C# would require a
`ReadOnlyView<T>` projection that exposes only read methods. We omit this
to keep the model small and protocol-focused.

---

## 12. Portability notes

The sealed `Status` hierarchy is the part most likely to need adapting in
other languages. The C# encoding pays a heap allocation per CAS attempt
(boxing the variant); this is acceptable for a teaching model but a
production port should use a flat representation.

### Recommended encoding by language

- **Rust**: a flat 6-variant enum. Wait/Ready/ReadAvailable/
  ChainClosed are unit variants; NextReader and NextWriter carry a raw
  pointer (`*const Slot` / `*const Behaviour`). Wrap in `AtomicU64` (or
  `AtomicPtr` with low-bit tagging if you want a single-word repr — that
  matches the C++ but is an optimisation, not the primary design).
  Status publication MUST use `Release` ordering; spin reads MUST use
  `Acquire`. The plain `ChainClosed` stores in StartEnqueue rely on the
  subsequent CAS in the reader's Release establishing happens-before
  — in Rust this must be an explicit `store(Release)`.
- **Scala**: sealed trait + case classes/objects. Same shape as the C#
  model. Use `AtomicReference[Status]` with `compareAndSet`. The
  read-only access type can be a `sealed view trait` parameterised on the
  cown so that the body signature genuinely cannot mutate (unlike C# and
  Python, which rely on convention).
- **Python**: a normal `Status` class with subclasses, guarded by a
  `threading.Lock` per Request. CAS via `lock + compare + assign`. The
  protocol is unchanged; the GIL provides much of what `Volatile.Read`
  gives in C#. Read-only enforcement is convention only.
- **C++ (production)**: the existing `Slot::status` low-bit-tagged
  `atomic<uintptr_t>` in `boc/behaviourcore.h`. `Slot::release` must be
  `noexcept`; under panic-equivalent failure the runtime should abort
  rather than unwind.

### Things easy to miss in a port

1. **`Cown<T>.value` visibility.** The body reads
   `Cown<T>.value` after `ResolveOne` enters the body thunk. The
   happens-before chain that makes this safe goes through the slot's
   `status` atomic (publish in FinishEnqueue, observe in the predecessor's
   Release path). A Rust/C++ port MUST use Acquire/Release ordering on
   `status` so the value writes from the previous writer are visible to
   the next reader/writer body.
2. **Singleton identity for unit variants.** `WaitStatus.Instance` etc. are
   singletons; CAS uses reference equality. In Rust/C++ this becomes
   "the unit variants compare by tag", which is automatic with `enum` but
   needs care if you encode states as separate pointer values.
3. **The bit-clear in `ReleaseRead`'s `LastReaderWaitingWriter` path**
   ([§6](#6-last-reader-wakes-writer)): easy to miss in a port — be careful
   when adapting.
4. **Sort comparator polarity.** Writers must sort BEFORE readers within
   a duplicate-cown group so the dedup pass keeps the writer slot. C#
   `(x.isRead).CompareTo(y.isRead)` returns negative when x is writer
   (`false < true`); negating flips the order.
5. **Reserved-word collisions.** `when` is fine in C# but collides with
   Scala/F#/Rust patterns; `read` collides with Rust's `std::io::Read`
   and Python's builtins. Use namespaces / qualified imports.
6. **`Release` must not unwind.** A panic in a slot's Release path leaves
   the cown queue in an unrecoverable state. Rust: `panic = abort`. C++:
   `noexcept`.

---

## See also

- [`When.cs`](When.cs) — the executable C# model.
- [`CPP_MAPPING.md`](CPP_MAPPING.md) — table mapping each C# type / method
  in `When.cs` to its C++ counterpart in `src/rt/boc/`.
- [`Program.cs`](Program.cs) — example/driver exercising all of the
  scenarios above.
- [`../modelimpl/`](../modelimpl) — the simpler **exclusive-access** model
  from the OOPSLA '23 paper. Recommended starting point; this read-only
  model builds directly on top of it. `Terminator.cs` and `StableOrder.cs`
  are reused from that directory via `<Compile Link>` in
  [`verona-csharp.csproj`](verona-csharp.csproj).
- [`behaviourcore.h`](../../../../src/rt/boc/behaviourcore.h) — the
  production implementation. See `Slot::release` (~line 1289) for the
  cascade walker, and `BehaviourCore::schedule_many` for full multi-schedule.
- [`cown.h`](../../../../src/rt/boc/cown.h) — `ReadRefCount` (~lines 46-121);
  line 85 is the count-clear in `release_read` discussed in
  [§6](#6-last-reader-wakes-writer).
- The original [BoC paper][bocpaper] for the write-only protocol.
