# Integrating an object model with BoC

The runtime's Behaviour-Oriented Concurrency (BoC) implementation does not
require cowns (concurrent owners) to use the Verona object representation. The
generic protocol in `src/rt/boc/` requires only a cown type, access to its
scheduling state, and operations for identity and lifetime management.

This tutorial builds a minimal object model on that protocol. It then explains
which parts belong in a production language-runtime adapter.

The complete minimal example is also available as
[`test/func/object-model/object-model.cc`](../../../test/func/object-model/object-model.cc).

## 1. Integration layers

A BoC integration has three layers:

1. **Generic BoC protocol.** `boc::BehaviourCore<ObjectModel>` and
   `boc::Slot<ObjectModel>` implement atomic acquisition, queueing, read
   sharing, and behaviour scheduling.
2. **Object-model adapter.** A small type tells the protocol how to find a
   cown's `CownSchedulerState`, retain and release the cown, and obtain its
   identity.
3. **Language-facing API.** A wrapper such as C++ `when` constructs behaviour
   bodies, creates slots, selects read or write access, and invokes the body.

Only the second and third layers are specific to a new object model. The
scheduler remains unchanged because `BehaviourCore` presents each ready
behaviour as a type-erased `Work` item.

## 2. The object-model contract

An object-model adapter must provide the following interface:

```cpp
struct ObjectModel
{
  using Cown = /* the model's cown representation */;

  static CownSchedulerState<ObjectModel>&
  get_cown_scheduler_state(Cown&) noexcept;

  static void acquire(Cown&) noexcept;
  static void release(Cown&) noexcept;

  static uintptr_t get_cown_identity(const Cown&) noexcept;
};
```

Each operation has one protocol responsibility:

| Operation | Responsibility |
|-----------|----------------|
| `Cown` | Names the pointee type referenced by each `Slot`. |
| `get_cown_scheduler_state` | Returns the object-model-owned, BoC-controlled state associated with that cown. |
| `acquire` | Thread-safely keeps the cown alive while scheduled behaviours refer to it. |
| `release` | Thread-safely drops a reference retained or transferred to the protocol. |
| `get_cown_identity` | Supplies a stable, unique ordering key for systematic testing. |

The adapter exposes `CownSchedulerState` but must not interpret its fields.
`BehaviourCore` and `Slot` are the only types that manipulate the queue and
reader/writer state. Each cown must return the same dedicated state throughout
its scheduling lifetime. That state must be initially empty and must not be
shared with another cown or reused while queued slots can still refer to it.

All four adapter operations must be non-throwing. They execute inside
scheduling and release paths that provide no exception rollback; later calls
can occur after the protocol has mutated queue or ownership state. An object
model that can fail during a lifetime operation must handle that failure
internally or terminate through its normal fatal-error policy.

All four operations must also be data-race-free when called concurrently.
`get_cown_scheduler_state` must return the stable state association without
unsafe lazy mutation. `get_cown_identity` must read an immutable identity or
otherwise synchronize access to it.

## 3. Define a cown representation

The smallest useful cown embeds scheduler state and implements some form of
lifetime management:

```cpp
#include <boc/behaviourcore.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

using namespace verona::rt;

struct ExampleObjectModel;

struct alignas(8) ExampleCown
{
  CownSchedulerState<ExampleObjectModel> scheduler_state;
  std::atomic<size_t> references{1};
};
```

The actual pointer passed to `boc::Slot` must have its low three bits clear
because the slot stores three flags in those bits. `alignas(8)` ensures that
ordinary `ExampleCown*` values satisfy this requirement; an integration must
not pass a tagged or adjusted pointer that violates it.

`CownSchedulerState` contains the per-cown state used by the BoC protocol:

- the tail of the cown's slot queue;
- the next writer waiting behind active readers;
- the active-reader count and writer-waiting bit.

Embedding one state in each cown is the recommended representation, but it is
not mechanically required. An object model may store the state elsewhere if
`get_cown_scheduler_state` preserves the same one-to-one association and
lifetime. The object model owns the storage, but BoC owns the meaning and
transitions of the fields.

## 4. Implement the adapter

The adapter connects `ExampleCown` to the generic protocol:

```cpp
struct ExampleObjectModel
{
  using Cown = ExampleCown;

  static CownSchedulerState<ExampleObjectModel>&
  get_cown_scheduler_state(Cown& cown) noexcept
  {
    return cown.scheduler_state;
  }

  static void acquire(Cown& cown) noexcept
  {
    cown.references.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(Cown& cown) noexcept
  {
    auto previous =
      cown.references.fetch_sub(1, std::memory_order_relaxed);
    assert(previous > 0);
  }

  static uintptr_t get_cown_identity(const Cown& cown) noexcept
  {
    return reinterpret_cast<uintptr_t>(&cown);
  }
};
```

This reference count is sufficient for the example because the cown has stack
storage and the count is used only to check balanced protocol operations. A
production implementation must connect `acquire` and `release` to its actual
object-lifetime mechanism. Both operations can run concurrently on scheduler
threads. `release` can also run during scheduling when the caller transferred
more ownership credits than a cown queue chain needs. It may destroy or reclaim
the cown only on the final release, after no remaining scheduler-state access
is possible.

The cown address is represented in each slot as a raw pointer. Ownership is
tracked separately: a request marked as transferred carries one ownership
credit until scheduling reconciles it. The pointer must be canonical,
meaning the unique pointer representation used for every request to that live
cown. It must also be non-null and unchanged while any behaviour is being
scheduled, queued, or executed. The identity must remain stable over that
period, must
distinguish simultaneously live cowns, and must order them consistently. Under
systematic testing, identity equality must mean that two slots refer to the
same canonical cown.

For repeated requests within one behaviour, only one representative slot owns
the queue interaction. Duplicate slots retain the tagged pointer value but
`Slot::release` ignores them without dereferencing the cown. The representative
slot's final `release` may therefore reclaim the cown before later duplicate
slots are visited. Destruction must not require any subsequent access to the
object or its scheduler state.

A suitably aligned address is therefore sufficient for a non-moving object
model. A moving object model cannot fix the raw-pointer requirement merely by
returning a stable numeric identity; its `Cown` type must instead be a pinned
object, stable handle, or other non-moving proxy.

## 5. Instantiate the BoC types

Specialise the two generic protocol types through aliases:

```cpp
using ExampleBehaviour = boc::BehaviourCore<ExampleObjectModel>;
```

This alias is the compiler-facing scheduling API for this object model. A
production integration would normally wrap it in generated helper functions or
a language-facing `when` API.

## 6. Define a behaviour body

`BehaviourCore::make` allocates body storage after the behaviour's slots
and aligns it as requested. The scheduler invokes the supplied
`void(Work*) noexcept` entry point when all requested cowns are available.

```cpp
struct alignas(64) Body
{
  std::atomic<size_t>* execution_count;
  ExampleCown* expected_cown;
};

void invoke(Work* work) noexcept
{
  auto* behaviour = ExampleBehaviour::from_work(work);
  auto* body = behaviour->get_body<Body>();

  assert(behaviour->acquired_cown(0) == body->expected_cown);
  assert(behaviour->acquired_mode(0) == AccessMode::Write);
  body->execution_count->fetch_add(1, std::memory_order_relaxed);

  body->~Body();
  ExampleBehaviour::finished(work);
}
```

This generated entry function has three responsibilities:

1. recover the specialised `BehaviourCore` and body;
2. run and destroy the body;
3. call `finished`, which releases the acquired requests and deallocates the
   behaviour.

Omitting `finished` leaves the cowns' queues blocked and leaks the behaviour
allocation. Calling it before the body finishes would release the
behaviour's acquired read or write access too early. `finished` does not run
the body destructor, so every normal and recoverable error path must destroy
the body and call `finished`. An entry point may omit `finished` only when it
intentionally reschedules the same `Work` and will complete it later.

An exception must not unwind out of the entry point through `Work::run`; the
scheduler does not provide an exception boundary. An adapter that permits
exceptions must catch them inside its entry point, destroy the body, and call
`finished` before applying the language runtime's error policy. A runtime that
cannot recover may instead terminate through its normal fatal-error path
without calling `finished`, but it must neither unwind through `Work::run` nor
continue execution with the affected queues blocked.

Body destruction must also be non-throwing. If a language adapter can observe a
destruction failure, it must catch that failure inside the entry point, call
`finished`, and then apply a non-unwinding error policy.

If the body needs its acquired cowns, it can retrieve them from
`acquired_cown` before calling `finished`. Those pointers must not be used
through behaviour-granted access after `finished` releases the requests.

## 7. Construct and schedule a behaviour

Initialise the existing scheduler, allocate a construction, initialise its
requests and body, finish construction, and submit the behaviour:

```cpp
#include <sched/schedulerthread.h>

int main()
{
  auto& scheduler = Scheduler::get();
  scheduler.init(2);

  ExampleCown cown;
  std::atomic<size_t> execution_count{0};

  auto construction = ExampleBehaviour::make(
    1, sizeof(Body), alignof(Body), invoke);

  ExampleBehaviour::initialise_request(
    construction,
    0,
    &cown,
    AccessMode::Write,
    Ownership::Borrowed);

  new (construction.body) Body{&execution_count, &cown};

  auto* behaviour =
    ExampleBehaviour::finish_construction(construction);

  // The caller keeps cown alive until schedule has consumed the raw pointer.
  ExampleBehaviour::schedule(behaviour);

  scheduler.run();

  assert(execution_count.load(std::memory_order_relaxed) == 1);
  assert(cown.references.load(std::memory_order_relaxed) == 1);
  heap::debug_check_empty();
}
```

The first argument to `make` is the number of cown requests.
`finish_construction` invalidates the construction token and returns the
schedulable behaviour. A compiler can schedule several completed behaviours
atomically with:

```cpp
ExampleBehaviour* batch[] = {first, second};
ExampleBehaviour::schedule(batch, 2);
```

For each shared cown, atomic batch scheduling prevents external requests from
interleaving between the batch's entries in that cown's queue. It does not make
the behaviour bodies execute simultaneously or transactionally.

Before `schedule` returns, slots contain raw pointers for which the protocol
has not yet necessarily established its own retained references. The caller
must therefore keep every cown alive until `schedule` returns, either through
an independently owned reference or through a reference explicitly transferred
with `Ownership::Transferred`, described next. A deferred batching API must
extend that lifetime until the batch is submitted, as C++ `when` does with its
`Batch` lifetime requirement.

If body construction fails, generated cleanup must destroy the constructed
behaviour body and call:

```cpp
ExampleBehaviour::abort(construction);
```

Ownership marked as transferred does not commit until scheduling begins, so an
aborted construction does not consume the caller's references.

## 8. Select read, write, and transferred access

Each request states its access and ownership independently:

```cpp
ExampleBehaviour::initialise_request(
  construction,
  index,
  &cown,
  AccessMode::Read,
  Ownership::Transferred);
```

`AccessMode::Read` changes scheduling semantics. The language-facing API must also
prevent the behaviour body from mutating the cown through that request. The C++
adapter exposes `const` access, which provides shallow C++ constness rather
than deep language-level immutability. The generic protocol cannot enforce an
object model's access rules.

An **ownership credit** is one reference that scheduling may retain for queued
work or release if it is surplus. `Ownership::Transferred` transfers one such
credit from the caller when scheduling begins. The caller must then relinquish
that owned reference and must not later release it as caller-owned.
The protocol reconciles transferred credits across all requests for the same
cown; it can release surplus credits during `schedule`. Without this flag, the
protocol obtains the references it needs by calling `ObjectModel::acquire`.

Within one behaviour, repeated occurrences of the same canonical cown pointer
are combined, and a write request dominates read requests for that cown.
Requests made by different behaviours remain distinct and form a per-cown
queue in batch order. Pointer equality identifies requests for the same cown;
systematic-testing builds use `get_cown_identity` to sort them
deterministically before applying the same grouping.

Read and write are access modes; borrowing and transfer are ownership modes. A
request can therefore be a borrowed read, transferred read, borrowed write, or
transferred write.

## 9. Add a language-facing `when` API

The compiler-facing API exposes the `Work*` entry convention but hides slot
layout and flag encoding. A source-language integration will normally generate
small helper functions around this API or place a higher-level `when` wrapper
above it.

Such a wrapper should:

1. accept the language's cown handles and behaviour body;
2. retain or transfer the handles according to the language's ownership
   rules;
3. call `BehaviourCore::make`;
4. initialise each request with explicit access and ownership modes;
5. construct the behaviour body, including any captured handles;
6. call `finish_construction`;
7. keep borrowed cowns alive until the batch is submitted;
8. call the appropriate `schedule` overload;
9. invoke and destroy the body before calling `BehaviourCore::finished`,
   including inside any language-level error or exception boundary;
10. ensure body destruction cannot unwind past `finished`;
11. prevent exceptions from unwinding through the scheduler's `Work::run`.

Body construction also needs an explicit failure policy. The simplest adapter
requires body construction to be non-throwing. An adapter that
permits construction failure needs an RAII guard covering the partially built
behaviour. Before propagating or handling the failure, the guard must destroy
the constructed behaviour body and call `BehaviourCore::abort`. Because transfer
commits at scheduling, the caller still owns every requested reference during
construction.

The existing C++ adapters provide two examples:

- [`src/rt/cpp/behaviour.h`](../../../src/rt/cpp/behaviour.h) is the
  lower-level Verona-specific body wrapper over `BehaviourCore`.
- [`src/rt/cpp/when.h`](../../../src/rt/cpp/when.h) provides the current typed
  C++ `when` interface, including read-only access and repeated cowns.

These files are structural examples of body and slot construction. They do
not currently provide the construction rollback or entry-point exception
boundaries described above, so an adapter that permits C++ exceptions must add
those mechanisms rather than copying the failure paths unchanged.

The wrapper is object-model-specific; the scheduler and BoC queueing protocol
are not.

## 10. Build and run the repository example

The repository example is built in both concurrent and systematic-testing
modes as part of `rt_tests`:

```sh
cmake -S . -B build_ninja -GNinja -DCMAKE_BUILD_TYPE=Debug
cmake --build build_ninja --target rt_tests -j2

build_ninja/test/func-con-object-model
build_ninja/test/func-sys-object-model
```

See [`docs/building.md`](../../building.md) for other platforms and build
configurations.

The example verifies that a cown which does not derive from Verona `Object` or
`Shared` can use the same `BehaviourCore` and scheduler. The focused
[`duplicate-cown-release`](../../../test/func/duplicate-cown-release/duplicate-cown-release.cc)
test checks transferred duplicate requests where the representative slot
performs final logical reclamation before the duplicate slot is released.

## Integration checklist

Before using a new object model in production, verify that:

- each pointer passed to a slot is canonical, non-null, stable, and has its low
  three bits clear;
- every live cown has a distinct, stable ordering identity;
- every object-model operation is data-race-free under concurrent invocation;
- `acquire` and `release` preserve the model's lifetime invariants;
- every object-model operation is non-throwing;
- each cown has one dedicated, stable
  `CownSchedulerState<ObjectModel>`;
- callers keep cowns alive until immediate or deferred batch submission;
- body alignment is a non-zero power of two and is passed consistently to
  `make` and `get_body`;
- body construction is non-throwing or protected by complete rollback;
- body destruction is non-throwing or handled before `finished`;
- body destruction happens before `BehaviourCore::finished`;
- every normal and recoverable error path eventually calls `finished`, unless
  the same work item is intentionally rescheduled;
- unrecoverable errors terminate without unwinding through `Work::run`;
- read-only slots expose only read-only language access;
- transferred slots correspond to references the caller owns and relinquishes;
- concurrent and systematic-testing builds exercise single-cown,
  multi-cown, duplicate, read/write, and destruction cases.
