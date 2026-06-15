// =============================================================================
//  C# model of Behaviour-Oriented Concurrency with read-only cowns.
//
//  This file extends the original write-only model (../modelimpl/) with
//  read-mode acquisition. Duplicate cowns (`when(a, a)`, `when(a, read(a))`,
//  `when(read(a), read(a))`) are supported via the sort + dedup pass in
//  `Behaviour.Schedule`.
//
//  See README.md for the protocol explainer (state diagrams, race traces,
//  worked examples) and CPP_MAPPING.md for the mapping back to the
//  production C++ implementation.
// =============================================================================


// -----------------------------------------------------------------------------
//  Status:  the 6-state MCS-queue state machine for a Request.
//
//  Encoded here as a sealed sum type: each state is its own subclass, with
//  the two non-payload-free variants (`NextReader`, `NextWriter`) carrying
//  the link target. CAS is `Interlocked.CompareExchange<Status>(ref status,
//  newValue, expectedValue)` -- a single word-sized atomic on the object
//  reference. The runtime type IS the tag.
//
//  A production implementation would tag the low bits of a single
//  pointer-sized atomic to fold the four payload-free states into the
//  pointer encoding and avoid the per-CAS allocation. That is an
//  optimisation of this representation, not a different protocol.
//
//  States:
//    Wait          -- 2PL not complete; successors must spin.
//    Ready         -- 2PL complete, no successor yet. THE ONLY CONTENDED STATE.
//    ReadAvailable -- this slot is a live reader; future readers may join the
//                     chain by closing it on themselves.
//    ChainClosed   -- a successor reader observed `ReadAvailable` on us and
//                     took over the chain; our `Release` will not notify next.
//    NextReader(r) -- successor is a reader Request `r`.
//    NextWriter(b) -- successor is a writer Behaviour `b`.
//
//  Transitions (legal):
//    Wait           -> Ready                by self (phase 3, non-head)
//    Wait           -> ReadAvailable        by self (phase 3, head reader)
//    Ready          -> NextReader(r)        by successor (phase 2 read, CAS)
//    Ready          -> NextWriter(b)        by successor (phase 2 write, CAS)
//    Ready          -> ReadAvailable        by writer cascade (CAS, races above)
//    ReadAvailable  -> ChainClosed          by successor whose CAS to NextReader/Writer failed
//
//  Terminal: ChainClosed, NextReader(_), NextWriter(_) are all "we have a
//  successor response, Release can act on it".
//
//  Notes:
//    * The four payload-free states are singletons. CAS uses reference
//      equality, so the `expected` argument to a CAS helper must ALWAYS be
//      one of those singleton instances -- a previously-read
//      NextReader/NextWriter value would never compare equal to a freshly
//      allocated one.
//    * NextReader carries a Request, NextWriter carries a Behaviour. This
//      reflects how each link is used: when the writer-cascade walker
//      reaches a NextReader it MUST read that reader's `status` field
//      (to find what is next behind it), so it needs the Request. When
//      the walker reaches a NextWriter it STOPS -- the writer is parked,
//      not part of the chain -- so all the walker needs is a Behaviour
//      reference to schedule later.
// -----------------------------------------------------------------------------

internal abstract class Status
{
    private protected Status() { }
}

internal sealed class WaitStatus : Status
{
    internal static readonly WaitStatus Instance = new WaitStatus();
    private WaitStatus() { }
}

internal sealed class ReadyStatus : Status
{
    internal static readonly ReadyStatus Instance = new ReadyStatus();
    private ReadyStatus() { }
}

internal sealed class ReadAvailableStatus : Status
{
    internal static readonly ReadAvailableStatus Instance = new ReadAvailableStatus();
    private ReadAvailableStatus() { }
}

internal sealed class ChainClosedStatus : Status
{
    internal static readonly ChainClosedStatus Instance = new ChainClosedStatus();
    private ChainClosedStatus() { }
}

internal sealed class NextReader : Status
{
    internal readonly Request Next;
    internal NextReader(Request r) { Next = r; }
}

internal sealed class NextWriter : Status
{
    internal readonly Behaviour Next;
    internal NextWriter(Behaviour b) { Next = b; }
}


/// <summary>
///   Reader/writer counter packed into one atomic int.
/// </summary>
/// <remarks>
///   Encoding:
///     count == 0          -- no readers, no writer waiting; cown idle for reads
///     count == 2n (n > 0) -- n readers reading
///     count == 2n + 1     -- n readers reading AND a writer is parked behind them
///                            (n may be zero: count == 1 means "writer parked,
///                            no readers" -- a transient state that
///                            <see cref="TryWrite"/> resolves before returning)
///
///   The "writer waiting" bit and the reader count share one atomic word
///   so that the two interesting races collapse to a single atomic op.
///   <para>
///   <b>Reader vs reader.</b> Two readers in <see cref="ReleaseRead"/>
///   each do <c>Interlocked.Add(-2)</c> on the count. Hardware linearises
///   the two adds: exactly one sees the post-decrement value reach the
///   "no readers" line and learns "I was the last reader"; the other sees
///   a non-zero value and learns "I'm not last". This case alone does not
///   require packing -- a single atomic counter would do.
///   </para>
///   <para>
///   <b>Reader vs writer.</b> This is the case that requires packing. The
///   writer's <see cref="TryWrite"/> wants to (a) mark itself as waiting
///   AND (b) decide whether the readers have already drained. The
///   reader's <see cref="ReleaseRead"/> wants to (c) decrement the count
///   AND (d) check whether a writer is waiting behind it. With one packed
///   word, the writer's <c>Add(+1)</c> does (a) and (b) in one atomic op
///   (if the result equals <c>WRITER_WAITING_BIT</c>, the readers are
///   already gone); the reader's <c>Add(-2)</c> does (c) and (d) in one
///   atomic op (the writer-waiting bit being set is observed atomically
///   with the count crossing zero).
///   </para>
///   <para>
///   With separate <c>readers</c> and <c>writerWaiting</c> fields, both
///   sides need two ops, and the following interleaving lets each side
///   think it owns the wake-up:
///   <list type="number">
///     <item>Writer sets <c>writerWaiting = true</c>.</item>
///     <item>Last reader decrements <c>readers</c> to 0.</item>
///     <item>Reader reads <c>writerWaiting = true</c> and wakes the
///     writer.</item>
///     <item>Writer reads <c>readers = 0</c>, takes its fast path, and
///     runs itself.</item>
///   </list>
///   The writer now runs twice: once via the fast path and again via the
///   reader's wake-up. Packing the bit into the same word as the count
///   eliminates that window because the atomic that sets/clears the bit
///   is the same atomic that observes the count.
///   </para>
/// </remarks>
internal sealed class ReadRefCount
{
    private const int READER_INCREMENT = 2;
    private const int WRITER_WAITING_BIT = 1;

    private int count = 0;

    internal enum DropResult { NotLast, LastReader, LastReaderWaitingWriter }

    /// <summary>
    ///   Register `n` new readers. Returns true if this is the first reader
    ///   (the count was zero before; the caller is now responsible for the
    ///   eventual `Release` of the read chain).
    /// </summary>
    internal bool AddRead(int n = 1)
    {
        int delta = n * READER_INCREMENT;
        int previous = Interlocked.Add(ref count, delta) - delta;
        return previous == 0;
    }

    /// <summary>
    ///   Decrement one reader. Returns whether we were the last reader and,
    ///   if so, whether a writer was parked waiting.
    /// </summary>
    /// <remarks>
    ///   In the <see cref="DropResult.LastReaderWaitingWriter"/> case, the
    ///   writer-waiting bit is cleared back to zero before returning, so
    ///   that the cown's counter is left empty for the writer to claim
    ///   exclusively. The clear uses <see cref="Volatile.Write"/>: the
    ///   handoff is published through the cown's MCS-queue atomics in
    ///   <c>WakeupNextWriter</c>, not through this field, so a plain
    ///   release-store is sufficient here.
    /// </remarks>
    internal DropResult ReleaseRead()
    {
        int newValue = Interlocked.Add(ref count, -READER_INCREMENT);
        if (newValue == WRITER_WAITING_BIT)
        {
            // We were the last reader AND a writer is parked. Clear the bit
            // so the writer runs against an empty counter.
            Volatile.Write(ref count, 0);
            return DropResult.LastReaderWaitingWriter;
        }
        if (newValue == 0) return DropResult.LastReader;
        return DropResult.NotLast;
    }

    /// <summary>
    ///   Attempt to claim the cown for write. Returns true if the count was
    ///   zero (no readers; the writer can run NOW) and leaves the count at
    ///   zero. Returns false if readers are still active; in that case the
    ///   writer-waiting bit is set, and the last reader will observe
    ///   <see cref="DropResult.LastReaderWaitingWriter"/> on its release.
    /// </summary>
    /// <remarks>
    ///   Precondition: not called in parallel with itself or
    ///   <see cref="AddRead"/> -- the MCS queue serialises writer slots and
    ///   ensures no new reader is added while a writer is parked.
    /// </remarks>
    internal bool TryWrite()
    {
        // Fast path: no readers, no writer waiting -> writer can run.
        if (Volatile.Read(ref count) == 0) return true;

        // Mark writer-waiting bit. Atomic add is safe even if a reader
        // releases concurrently: the bit lives in the low bit, the reader
        // counter in the high bits, so they never overlap.
        int newValue = Interlocked.Add(ref count, WRITER_WAITING_BIT);

        if (newValue == WRITER_WAITING_BIT)
        {
            // Between the load and the add, the last reader released. The
            // count was 0 when we added the bit, so it is 1 now with no
            // readers active. Clear the bit and run.
            Volatile.Write(ref count, 0);
            return true;
        }
        return false;
    }
}


/// <summary>
///   Common part of a cown that is independent of the data the cown is storing.
/// </summary>
/// <remarks>
///   Holds the MCS-queue tail (<see cref="last"/>), the parked-next-writer
///   pointer (<see cref="nextWriter"/>), and the read counter
///   (<see cref="readRefCount"/>).
/// </remarks>
class CownBase : StableOrder
{
    /// <summary>
    ///   The end of the queue of requests for this cown. Null when no
    ///   behaviour is currently scheduled on this cown.
    /// </summary>
    /// <remarks>
    ///   Accessed through <see cref="Interlocked"/> primitives only, so
    ///   <c>volatile</c> is unnecessary (and would conflict with
    ///   <c>ref</c>-pass to Interlocked).
    /// </remarks>
    internal Request? last = null;

    /// <summary>
    ///   The writer parked behind a live read chain, to be woken by the last
    ///   reader. Null when no writer is waiting.
    /// </summary>
    internal Behaviour? nextWriter = null;

    /// <summary>
    ///   Atomic packed counter of active readers + writer-waiting bit.
    ///   See <see cref="ReadRefCount"/>.
    /// </summary>
    internal readonly ReadRefCount readRefCount = new ReadRefCount();

    /// <summary>
    ///   Resolve the parked next writer. Called by the last reader of a
    ///   chain when <c>ReleaseRead</c> returns <c>LastReaderWaitingWriter</c>.
    /// </summary>
    internal void WakeupNextWriter()
    {
        // The writer may not yet have published itself: the racing thread
        // is between `TryWrite` (which set the writer-waiting bit) and
        // its `Volatile.Write` to `nextWriter`. Spin until it appears.
        var w = new SpinWait();
        Behaviour? b;
        while ((b = Volatile.Read(ref nextWriter)) == null) { w.SpinOnce(); }

        // Clear the field so the spin above starts from null next time a
        // writer parks. No contention here: the writer-waiting bit was
        // cleared by our caller (`ReleaseRead`) before this call, and a
        // new writer cannot park until the bit is re-armed by a fresh
        // `TryWrite`, which requires a new chain to form first.
        nextWriter = null;
        b.ResolveOne();
    }
}


/// <summary>
///   An access request for a single cown, carrying its read/write mode.
///   Implemented by <see cref="Cown{T}"/> (write) and <see cref="ReadCown{T}"/>
///   (read).
/// </summary>
interface ICownAccess<T>
{
    CownBase Target { get; }
    bool IsRead { get; }

    /// <summary>
    ///   Read the underlying value. Implementations dispatch on the concrete
    ///   access type (write vs read); the body sees a raw <typeparamref name="T"/>
    ///   either way. Read access just grants concurrent visibility; the
    ///   body is expected by convention not to mutate. See
    ///   <c>README.md</c> §"What we left out".
    /// </summary>
    T Value { get; }
}


/// <summary>
///   Behaviour that captures the content of a `when` body.
/// </summary>
/// <remarks>
///   Holds the body thunk, the set of cown requests, and a `count` of
///   outstanding requests. Each request, once at the head of its cown's
///   queue, calls <see cref="ResolveOne"/> to decrement `count`; when count
///   reaches zero (after the initial 2PL-safety `+1` is also decremented in
///   <see cref="Schedule"/>) the thunk is dispatched.
/// </remarks>
class Behaviour
{
    Action thunk;
    int count;
    Request[] requests;

    /// <summary>
    ///   Construct a Behaviour for the given accesses. Sorts the requests
    ///   into deadlock-free order and deduplicates entries that refer to the
    ///   same cown.
    /// </summary>
    /// <param name="t">The body to run once all cowns are acquired.</param>
    /// <param name="accesses">
    ///   The cown accesses, in user-supplied order. Each `(target, isRead)`
    ///   becomes one <see cref="Request"/>; duplicates of the same `target`
    ///   are detected and collapsed in <see cref="Schedule"/>.
    /// </param>
    internal Behaviour(Action t, (CownBase target, bool isRead)[] accesses)
    {
        thunk = t;
        requests = new Request[accesses.Length];
        for (int i = 0; i < accesses.Length; i++)
        {
            requests[i] = new Request(accesses[i].target, accesses[i].isRead, this);
        }

        // Phase 1: sort + dedup.
        //
        // Sort by `(cown.identity, isRead)`. Within a duplicate-cown group,
        // writers sort BEFORE readers (false sorts before true); the dedup
        // pass below then keeps the first slot of each group, so a
        // `when(read(a), a)` collapses to a single write slot.
        Array.Sort(requests, (x, y) =>
        {
            int c = x.target.CompareTo(y.target);
            if (c != 0) return c;
            // Same cown: writer (isRead == false) sorts before reader.
            return x.isRead.CompareTo(y.isRead);
        });

        // Dedup pass: any slot whose cown matches its predecessor's is a
        // duplicate. Mark and decrement the pending-count so we don't wait
        // on ourselves.
        int duplicates = 0;
        for (int i = 1; i < requests.Length; i++)
        {
            if (object.ReferenceEquals(requests[i].target, requests[i - 1].target))
            {
                requests[i].isDuplicate = true;
                duplicates++;
            }
        }

        // `count + 1` is the 2PL safety extra (see `Schedule`).
        count = requests.Length - duplicates + 1;
    }

    /// <summary>
    ///   Schedule the behaviour, performing two-phase locking across all
    ///   non-duplicate requests.
    /// </summary>
    /// <remarks>
    ///   <para>
    ///     Phase 1 (sort + dedup) ran in this behaviour's constructor.
    ///     <c>Schedule</c> runs the remaining three phases over the sorted,
    ///     deduplicated request list:
    ///   </para>
    ///   <list type="number">
    ///     <item><b>Phase 2 (Acquire):</b> in sorted order, enqueue each
    ///     non-duplicate request on its cown. For a reader, this may also
    ///     do the contended CAS on the predecessor's status; if the
    ///     predecessor is already `ReadAvailable`, the reader joins the
    ///     live chain immediately by calling `AddRead` on the cown.</item>
    ///     <item><b>Phase 3 (Release of 2PL):</b> set each slot's status
    ///     to either `Ready` (we will be woken by predecessor) or
    ///     `ReadAvailable` (head-of-chain reader; future readers may join).
    ///     This is the signal to subsequent enqueuers that our 2PL is
    ///     complete.</item>
    ///     <item><b>Phase 4 (Resolve):</b> for each slot that can run now
    ///     (no predecessor, or joined a live read chain), arrange for
    ///     <see cref="ResolveOne"/> to be called: directly for readers,
    ///     via `TryWrite` for writers.</item>
    ///     <item>Finally <see cref="ResolveOne"/> on this behaviour itself
    ///     drops the 2PL-safety `+1`; if all real resolves already
    ///     happened, this is the one that dispatches the thunk.</item>
    ///   </list>
    /// </remarks>
    internal void Schedule()
    {
        // Phase 2: Acquire.
        foreach (var r in requests)
        {
            if (r.isDuplicate) continue;
            r.StartEnqueue();
        }

        // Phase 3: Release of 2PL (publish each slot's status).
        foreach (var r in requests)
        {
            if (r.isDuplicate) continue;
            r.FinishEnqueue();
        }

        // Phase 4: Resolve.
        foreach (var r in requests)
        {
            if (r.isDuplicate) continue;
            r.PostEnqueue();
        }

        // Hold the Terminator open before the 2PL-safety decrement below.
        // The `+1` in the constructor's count guarantees the body cannot
        // fire before the trailing ResolveOne, so this is the latest safe
        // point at which to register with the Terminator.
        Terminator.Increment();

        // 2PL-safety decrement (matches the `+1` in the constructor).
        ResolveOne();
    }

    /// <summary>
    ///   Drop one pending request. If this was the last (count hits zero),
    ///   dispatch the body on a worker thread; on completion, release every
    ///   slot, including duplicates (which are no-ops).
    /// </summary>
    internal void ResolveOne()
    {
        if (Interlocked.Decrement(ref count) != 0)
            return;

        Task.Run(() =>
        {
            try
            {
                thunk();
            }
            catch (Exception ex)
            {
                // Body exception policy. This model does not specify what
                // happens to the cowns this behaviour holds when the body
                // throws, so for clarity we just end the process.
                //
                // A production implementation has gentler options:
                //   * black-hole the cowns -- mark them poisoned so any
                //     subsequent behaviour acquiring them fails (or is
                //     silently dropped) instead of running against
                //     possibly-inconsistent state;
                //   * propagate the exception to a parent / supervising
                //     behaviour;
                //   * quarantine the transitively-reachable sub-graph and
                //     let the rest of the program keep running.
                //
                // None of those is a concern for the model.
                Environment.FailFast($"behaviour body threw: {ex}", ex);
            }

            foreach (var r in requests)
            {
                r.Release();
            }

            Terminator.Decrement();
        });
    }
}


/// <summary>
///   A request for a cown -- one queue node in the MCS-style chain.
/// </summary>
class Request
{
    /// <summary>The cown this request is for.</summary>
    internal readonly CownBase target;

    /// <summary>True for read access, false for write. Set at construction.</summary>
    internal readonly bool isRead;

    /// <summary>
    ///   The behaviour owning this request. Used by readers so that a
    ///   cascading writer can resolve them.
    /// </summary>
    internal readonly Behaviour behaviour;

    /// <summary>
    ///   True if this request was collapsed by the dedup pass in
    ///   <see cref="Behaviour"/>'s constructor (the same cown appears more
    ///   than once in the originating `when`). Duplicate requests do NOT
    ///   enter 2PL and their <see cref="Release"/> is a no-op.
    /// </summary>
    internal bool isDuplicate = false;

    /// <summary>
    ///   The 6-state MCS-queue state machine. See the doc-comment on
    ///   <see cref="Status"/> for the full transition table.
    /// </summary>
    /// <remarks>
    ///   This field is updated either by CAS through
    ///   <see cref="Interlocked.CompareExchange{T}(ref T, T, T)"/> (full
    ///   barrier) or by a plain assignment (publishing to a successor we
    ///   know is currently spinning on us; see the assignments to
    ///   <see cref="ChainClosedStatus"/> in <see cref="StartEnqueue"/>).
    ///   The <c>volatile</c> modifier triggers CS0420 when the field is
    ///   passed by reference to <c>Interlocked.CompareExchange</c>, so we
    ///   omit it and read explicitly through <see cref="Volatile.Read{T}"/>
    ///   on the spin sites instead. Acquire/release ordering for the
    ///   protocol is carried by the Interlocked CAS itself; plain reads
    ///   in the spin loops are intentional.
    ///
    ///   Porting note: the plain assignment publishing
    ///   <c>ChainClosedStatus.Instance</c> works in C# because every
    ///   subsequent reader of this field goes through an Interlocked CAS
    ///   (in <see cref="Release"/>'s tail spin), which establishes
    ///   happens-before. In a language with an explicit weak memory model
    ///   (Rust, C++, Java, Go, etc.) the corresponding store must use
    ///   release ordering and the matching spin-load must use acquire
    ///   ordering so the walker still synchronises.
    /// </remarks>
    internal Status status = WaitStatus.Instance;

    // ---- transient flag set in StartEnqueue, consumed by FinishEnqueue/PostEnqueue:

    /// <summary>
    ///   True when no predecessor's <see cref="Release"/> will resolve us,
    ///   so <see cref="PostEnqueue"/> is responsible for our fate.
    ///   Set in <see cref="StartEnqueue"/> in three situations:
    ///     <list type="bullet">
    ///       <item>no predecessor at all (we are the head of the cown's
    ///       queue);</item>
    ///       <item>a reader joining a live read chain (predecessor was
    ///       <see cref="ReadAvailableStatus"/>, so we closed the chain on
    ///       it and <see cref="ReadRefCount.AddRead"/>'d ourselves into
    ///       the group);</item>
    ///       <item>a writer that closed the chain on a
    ///       <see cref="ReadAvailableStatus"/> predecessor; in that case
    ///       <see cref="PostEnqueue"/> calls <see cref="ReadRefCount.TryWrite"/>
    ///       which either runs us immediately (if all readers in the chain
    ///       have already left) or parks us as the cown's <c>nextWriter</c>
    ///       to be woken by the last reader's <see cref="Release"/>.</item>
    ///     </list>
    /// </summary>
    bool selfResolve = false;

    internal Request(CownBase t, bool read, Behaviour b)
    {
        target = t;
        isRead = read;
        behaviour = b;
    }

    // -------------------------------------------------------------------------
    //  Status transitions (CAS helpers).
    // -------------------------------------------------------------------------

    /// <summary>
    ///   CAS this slot's status from `Ready` to `NextReader(s)`. Returns
    ///   true on success. On failure (status is `ReadAvailable`), the
    ///   caller is expected to close the chain by storing `ChainClosed`
    ///   and to join the live read group via `readRefCount.AddRead`.
    /// </summary>
    bool TrySetNextReader(Request s)
    {
        return object.ReferenceEquals(
            Interlocked.CompareExchange(ref status, new NextReader(s), ReadyStatus.Instance),
            ReadyStatus.Instance);
    }

    /// <summary>
    ///   CAS this slot's status from `Ready` to `NextWriter(b)`. Returns
    ///   true on success. On failure (status is `ReadAvailable`), the
    ///   caller closes the chain and parks itself as `cown.nextWriter`.
    /// </summary>
    bool TrySetNextWriter(Behaviour b)
    {
        return object.ReferenceEquals(
            Interlocked.CompareExchange(ref status, new NextWriter(b), ReadyStatus.Instance),
            ReadyStatus.Instance);
    }

    /// <summary>
    ///   CAS this slot's status from `Ready` to `ReadAvailable`. Used by
    ///   the writer-wakes-readers cascade walker. Returns true on success;
    ///   on failure the walker reads our current status to learn who took
    ///   over the chain.
    /// </summary>
    bool TrySetReadAvailable()
    {
        return object.ReferenceEquals(
            Interlocked.CompareExchange(ref status, ReadAvailableStatus.Instance, ReadyStatus.Instance),
            ReadyStatus.Instance);
    }

    // -------------------------------------------------------------------------
    //  Phase 2: StartEnqueue.
    // -------------------------------------------------------------------------

    /// <summary>
    ///   Enqueue ourselves at the tail of <c>target</c>'s MCS queue and
    ///   wait for the predecessor's 2PL to complete. Sets
    ///   <see cref="selfResolve"/> when no predecessor's Release will
    ///   resolve us, so <see cref="FinishEnqueue"/> and
    ///   <see cref="PostEnqueue"/> can act on the outcome.
    /// </summary>
    internal void StartEnqueue()
    {
        var prev = Interlocked.Exchange(ref target.last, this);

        if (prev == null)
        {
            // No predecessor on this cown. Phase 3 will publish our status.
            selfResolve = true;

            if (isRead)
            {
                // Open a new read chain.
                target.readRefCount.AddRead(1);
            }
            return;
        }

        // 2PL wait: spin until the predecessor's FinishEnqueue has run.
        var w = new SpinWait();
        while (Volatile.Read(ref prev.status) is WaitStatus) { w.SpinOnce(); }

        if (isRead)
        {
            if (prev.TrySetNextReader(this))
            {
                // We are linked behind prev. prev's Release (or a writer's
                // cascade walking through prev) will resolve us. We will
                // be counted into readRefCount at that point, not now.
                return;
            }

            // CAS failed: prev.status is `ReadAvailable`. Close the chain
            // on prev (so its Release won't try to notify us) and join the
            // live read group.
            //
            // Plain store (no Interlocked): the only thread that observes
            // `prev.status == ReadAvailable` is the cascade walker on the
            // chain-opening writer, which reaches this slot through its
            // own CAS sequence and re-reads `prev.status` under that
            // happens-before. In a language with an explicit weak memory
            // model this store must use release ordering (see the
            // `status` field doc-comment).
            prev.status = ChainClosedStatus.Instance;
            target.readRefCount.AddRead(1);
            selfResolve = true;
            return;
        }

        // Writer.
        if (prev.TrySetNextWriter(behaviour))
        {
            // Linked behind prev; prev's Release will resolve our behaviour.
            return;
        }

        // CAS failed: prev.status is `ReadAvailable`. Close the chain on
        // prev. We become the parked next writer (set in PostEnqueue, after
        // FinishEnqueue has published our status). See note above on the
        // plain store and weak-memory porting.
        prev.status = ChainClosedStatus.Instance;
        selfResolve = true;
    }

    // -------------------------------------------------------------------------
    //  Phase 3: FinishEnqueue.
    // -------------------------------------------------------------------------

    /// <summary>
    ///   Publish this slot's status, signalling to subsequent enqueuers
    ///   that our 2PL is complete and they may proceed past us. A reader
    ///   with <see cref="selfResolve"/> publishes <c>ReadAvailable</c>;
    ///   everything else publishes <c>Ready</c>.
    /// </summary>
    internal void FinishEnqueue()
    {
        if (isRead && selfResolve)
        {
            status = ReadAvailableStatus.Instance;
        }
        else
        {
            status = ReadyStatus.Instance;
        }
    }

    // -------------------------------------------------------------------------
    //  Phase 4: PostEnqueue.
    // -------------------------------------------------------------------------

    /// <summary>
    ///   For slots that can run immediately (no predecessor, or joined a
    ///   live read chain), arrange for <see cref="Behaviour.ResolveOne"/>
    ///   to fire. Readers can always resolve (their `AddRead` happened in
    ///   `StartEnqueue`). Writers must check the reader counter: if no
    ///   readers active, resolve now; else park as `cown.nextWriter`.
    /// </summary>
    internal void PostEnqueue()
    {
        if (!selfResolve)
        {
            // We are linked behind a predecessor; their Release path will
            // resolve us.
            return;
        }

        // Readers always resolve here (their `AddRead` happened in
        // `StartEnqueue`). A writer must first claim the cown via
        // `TryWrite`: if no readers are active it wins and resolves now;
        // otherwise it parks itself for the last reader to wake.
        if (isRead || target.readRefCount.TryWrite())
        {
            behaviour.ResolveOne();
            return;
        }

        // Use Volatile.Write so the spin in WakeupNextWriter
        // (Volatile.Read) is guaranteed to observe this publish.
        Volatile.Write(ref target.nextWriter, behaviour);
    }

    // -------------------------------------------------------------------------
    //  Release: called from Behaviour.ResolveOne's worker on body completion.
    // -------------------------------------------------------------------------

    /// <summary>
    ///   Hand the cown to the next request, if any. Four principal cases:
    ///   <list type="bullet">
    ///     <item>No successor at all: CAS the cown's tail to null. If we
    ///     were a reader, also <see cref="DropRead"/>.</item>
    ///     <item>We are a reader: <see cref="DropRead"/> (which may wake
    ///     a parked writer if we are the last reader).</item>
    ///     <item>We are a writer with a writer successor
    ///     (<see cref="NextWriter"/>): resolve it directly.</item>
    ///     <item>We are a writer with a reader successor
    ///     (<see cref="NextReader"/>): cascade-walk forward, marking each
    ///     consecutive reader <c>ReadAvailable</c>, then resolve every
    ///     walked reader's behaviour.</item>
    ///   </list>
    /// </summary>
    internal void Release()
    {
        if (isDuplicate)
        {
            // The original write/read slot for this cown handles release.
            return;
        }

        var s = Volatile.Read(ref status);
        if (s is WaitStatus or ReadyStatus or ReadAvailableStatus)
        {
            // No successor has linked behind us yet. Try to leave the queue.
            var slot = this;
            if (Interlocked.CompareExchange(ref target.last, null, slot) == slot)
            {
                if (isRead)
                {
                    DropRead();
                }
                return;
            }

            // CAS failed: another thread is enqueueing behind us. Wait
            // until it has published a successor response on our status.
            var w = new SpinWait();
            while (true)
            {
                s = Volatile.Read(ref status);
                if (!(s is WaitStatus or ReadyStatus or ReadAvailableStatus)) break;
                w.SpinOnce();
            }
        }

        if (isRead)
        {
            // Reader's release: drop our share of the read count.
            // Whatever successor exists will be handled by either:
            //  - the chain still being open (we are NOT the last reader),
            //  - the last reader calling WakeupNextWriter (waiting writer), or
            //  - the next enqueuer arriving on an empty cown (no waiter).
            DropRead();
            return;
        }

        // Writer's release.
        if (s is NextWriter nw)
        {
            nw.Next.ResolveOne();
            return;
        }

        // Writer waking up at least one reader. Cascade-walk the chain.
        WakeupReaderChain((NextReader)s);
    }

    /// <summary>
    ///   Writer-wakes-readers cascade. Walk forward through consecutive
    ///   reader links from <paramref name="firstNext"/>, marking each with
    ///   `ReadAvailable`. Stop at the first reader we successfully marked
    ///   (subsequent readers will join the live chain via the contended
    ///   path) or at a writer (which we park as `cown.nextWriter`).
    /// </summary>
    void WakeupReaderChain(NextReader firstNext)
    {
        // Claim one read slot before publishing `ReadAvailable` on the
        // terminator. Without this up-front AddRead, a writer arriving
        // mid-cascade (observing `ReadAvailable` on its predecessor and
        // chain-closing) would see `readRefCount == 0`, take TryWrite's
        // fast path, and start executing concurrently with the readers
        // we are about to wake.
        target.readRefCount.AddRead(1);

        // Pass 1: walk the chain, marking each reader and counting them.
        // We do not resolve in this pass -- once a reader is resolved its
        // body may run and its slot become garbage at any moment, which
        // would invalidate the re-traversal in pass 2.
        Request curr = firstNext.Next;
        int readerCount = 0;
        Behaviour? trailingWriter = null;

        while (true)
        {
            // Spin while curr's phase-3 has not yet published a status:
            // without this guard, the subsequent pattern match would see
            // `WaitStatus` and mis-classify the slot as having a writer
            // successor (it matches neither NextReader nor NextWriter).
            // See README.md §7 for the race trace this guard fixes.
            var w = new SpinWait();
            while (Volatile.Read(ref curr.status) is WaitStatus) { w.SpinOnce(); }

            if (curr.TrySetReadAvailable())
            {
                // curr is now the head of the live read chain; later
                // readers will join via the contended CAS path.
                readerCount++;
                break;
            }

            // CAS failed: curr.status is something other than Ready.
            // It must be NextReader, NextWriter, or ReadAvailable (the
            // last can't happen here -- only we set ReadAvailable, and we
            // are the unique writer doing so).
            var cs = Volatile.Read(ref curr.status);
            switch (cs)
            {
                case NextReader nr:
                    readerCount++;
                    curr = nr.Next;
                    continue;
                case NextWriter nw:
                    readerCount++;
                    trailingWriter = nw.Next;
                    break;
            }
            break;
        }

        // Add reads for the additional readers we walked past (the first
        // reader was covered by AddRead(1) above; subsequent ones need
        // their own count).
        if (readerCount > 1)
        {
            target.readRefCount.AddRead(readerCount - 1);
        }

        if (trailingWriter != null)
        {
            // Park the trailing writer; some reader's DropRead will wake it.
            // There must be at least one reader (the chain we just opened),
            // so TryWrite must return false here.
            target.readRefCount.TryWrite();
            Volatile.Write(ref target.nextWriter, trailingWriter);
        }

        // Pass 2: re-traverse the chain, resolving each reader. NextReader
        // links are immutable once published (no status transition ever
        // overwrites a successor pointer), so reading them a second time
        // is safe. Capture each next link *before* calling ResolveOne --
        // the slot can become garbage as soon as its body runs and Release
        // fires.
        Request r = firstNext.Next;
        for (int i = 0; i < readerCount - 1; i++)
        {
            var next = ((NextReader)Volatile.Read(ref r.status)).Next;
            r.behaviour.ResolveOne();
            r = next;
        }
        r.behaviour.ResolveOne();
    }

    /// <summary>
    ///   Drop one reader from the active set; if we were the last and a
    ///   writer was parked, wake it.
    /// </summary>
    void DropRead()
    {
        var result = target.readRefCount.ReleaseRead();
        if (result == ReadRefCount.DropResult.LastReaderWaitingWriter)
        {
            target.WakeupNextWriter();
        }
        // result == NotLast: nothing to do; other readers carry on.
        // result == LastReader: queue is empty (no waiter); next enqueuer
        // will discover it by `Interlocked.Exchange` returning null.
    }
}


/// <summary>
///   Cown wrapping a value of type <typeparamref name="T"/>. The value
///   should only be accessed inside a <c>when()</c> body. Acquiring as
///   <c>read(c)</c> grants concurrent read access; acquiring directly
///   grants exclusive write access.
/// </summary>
class Cown<T> : CownBase, ICownAccess<T>
{
    internal T value;
    public Cown(T v) { value = v; }

    CownBase ICownAccess<T>.Target => this;
    bool ICownAccess<T>.IsRead => false;
    T ICownAccess<T>.Value => value;
}


/// <summary>
///   Read-mode marker for a cown, produced by <see cref="When.read{T}"/>.
/// </summary>
/// <remarks>
///   Read access grants concurrent visibility of the wrapped value but the
///   body is expected by convention not to mutate it. Enforcement is not
///   provided at the language level. See <c>README.md</c> §"What we left
///   out" for why.
/// </remarks>
class ReadCown<T> : ICownAccess<T>
{
    internal readonly Cown<T> cown;
    internal ReadCown(Cown<T> c) { cown = c; }

    CownBase ICownAccess<T>.Target => cown;
    bool ICownAccess<T>.IsRead => true;
    T ICownAccess<T>.Value => cown.value;
}


/// <summary>
///   Entry points for scheduling behaviours: the <c>when(...)</c> family
///   and the <c>read(c)</c> marker.
/// </summary>
/// <remarks>
///   Read and write mode share a single <see cref="ICownAccess{T}"/>
///   interface implemented by both <see cref="Cown{T}"/> (write) and
///   <see cref="ReadCown{T}"/> (read), so one <c>when</c> overload per
///   arity suffices.
/// </remarks>
class When
{
    /// <summary>Mark a cown for read access in a <c>when(...)</c> clause.</summary>
    public static ReadCown<T> read<T>(Cown<T> c) => new ReadCown<T>(c);

    public static Action<Action> when()
    {
        return f => new Behaviour(f, Array.Empty<(CownBase, bool)>()).Schedule();
    }

    public static Action<Action<T>> when<T>(ICownAccess<T> a)
    {
        return f =>
        {
            var thunk = () => f(a.Value);
            new Behaviour(thunk, new[] { (a.Target, a.IsRead) }).Schedule();
        };
    }

    public static Action<Action<T1, T2>> when<T1, T2>(ICownAccess<T1> a, ICownAccess<T2> b)
    {
        return f =>
        {
            var thunk = () => f(a.Value, b.Value);
            new Behaviour(thunk, new[] {
                (a.Target, a.IsRead),
                (b.Target, b.IsRead),
            }).Schedule();
        };
    }

    public static Action<Action<T1, T2, T3>> when<T1, T2, T3>(
        ICownAccess<T1> a, ICownAccess<T2> b, ICownAccess<T3> c)
    {
        return f =>
        {
            var thunk = () => f(a.Value, b.Value, c.Value);
            new Behaviour(thunk, new[] {
                (a.Target, a.IsRead),
                (b.Target, b.IsRead),
                (c.Target, c.IsRead),
            }).Schedule();
        };
    }
}
