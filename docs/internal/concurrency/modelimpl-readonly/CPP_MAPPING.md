# C++ ↔ C# correspondence

This table maps the C# model in this directory to the production
read-only-cowns implementation in `src/rt/boc/`. Use it as the bridge
when porting fixes in either direction.

## Things the C# model deliberately leaves out

The C# model omits one orthogonal protocol feature and two
production-implementation concerns:

1. **Atomic multi-schedule** (`when(a) + when(b) + when(a, b)`) — an
   additional feature of the C++ implementation, orthogonal to read-only
   cowns. Each `when` in the C# model creates one `Behaviour`; we never
   link chains across behaviours. The two features compose in the C++
   implementation, but the combination is what makes the production code
   significantly more complex than either feature alone.
2. **Reference counting** (`Cown::acquire` / `Cown::release`) — a
   production-implementation concern. C# uses GC; cowns stay reachable as
   long as in-flight `Behaviour`s hold a `Request` that points at them.
3. **The `exec_count_down` n == 1 shortcut** — a production
   optimisation. The C# `count + 1` trick plus `ResolveOne()` already
   encodes the 2PL safety decrement, so the shortcut buys nothing in the
   model.

## Type and method mapping

| C++ (`boc/`, `cpp/`)                     | C# (`When.cs`)                  |
|------------------------------------------|---------------------------------|
| `Cown` (`boc/cown.h`)                    | `CownBase`                      |
| `ActualCown<T>` (`cpp/cown.h`)           | `Cown<T>`                       |
| `cown_ptr<const T>` + `Access<const T>`  | `ReadCown<T>` + `read(c)`       |
| `BehaviourCore` (`boc/behaviourcore.h`)  | `Behaviour`                     |
| `Slot` (`boc/behaviourcore.h`)           | `Request`                       |
| `Slot::status` (6-state atomic word)     | `Request.status : Status`       |
| `STATUS_WAIT`                            | `WaitStatus.Instance`           |
| `STATUS_READY`                           | `ReadyStatus.Instance`          |
| `STATUS_READAVAILABLE`                   | `ReadAvailableStatus.Instance`  |
| `STATUS_CHAIN_CLOSED`                    | `ChainClosedStatus.Instance`    |
| reader-link bit pattern (`ptr | READ`)   | `NextReader(Request)`           |
| writer-link bit pattern (`ptr`)          | `NextWriter(Behaviour)`         |
| `COWN_READER_FLAG`                       | `Request.isRead`                |
| `COWN_DUPLICATE_FLAG`                    | `Request.isDuplicate`           |
| `Slot::behaviour` (reader back-pointer)  | `Request.behaviour`             |
| `Cown::next_writer`                      | `CownBase.nextWriter`           |
| `Cown::read_ref_count` (`ReadRefCount`)  | `CownBase.readRefCount`         |
| `cown->last_slot`                        | `CownBase.last`                 |
| `set_next_slot_reader_contended`         | `Request.TrySetNextReader`      |
| `set_next_slot_writer_contended`         | `Request.TrySetNextWriter`      |
| `set_read_available_contended`           | `Request.TrySetReadAvailable`   |
| `Slot::release`                          | `Request.Release`               |
| `Slot::drop_read`                        | `Request.DropRead`              |
| `Slot::wakeup_next_writer`               | `CownBase.WakeupNextWriter`     |
| `BehaviourCore::schedule_many`           | `Behaviour.Schedule`            |
| `Aal::pause()`                           | `SpinWait.SpinOnce()`           |

For the protocol explainer (state diagrams, race traces, worked examples,
portability notes) see [`README.md`](README.md). Inline `Mirrors ...`
comments throughout `When.cs` cite specific C++ functions and line
numbers for finer-grained cross-references.
