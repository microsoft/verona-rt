/**
 *  Used to provide a stable order over. It is based initially on hashCode. If there 
 *  is a hashcode collision, then we use a counter to disambiguate between
 *  the two objects. Alternatively, we could just use an atomic on allocation. Here
 *  the aim is to reduce the likelyhood of that atomic. 
 **/
class StableOrder : IComparable
{
    // Used to disambiguate between two cowns that have the same hashcode.
    private static volatile int counter = 0;

    // If we get a hashcode collision, then we need to use an additional value
    // to distinguish between the two objects. We don't want to use this from
    // the start as it requires Interlocked operations to create it.
    private Int64 collision = 0;

    private void EnsureCollisionSet()
    {
        if (collision == 0)
        {
            collision = Interlocked.Increment(ref counter);
        }
    }

    public int CompareTo(object? obj)
    {
        if (obj == null) return 1;

        var result = this.GetHashCode().CompareTo(obj.GetHashCode());
        if (result != 0) return result;

        if (obj is StableOrder sobj)
        {
            // We are comparing two cowns with the same hashcode. We need to
            // create a unique value for each cown to disambiguate between
            // them.
            EnsureCollisionSet();
            sobj.EnsureCollisionSet();

            return collision.CompareTo(sobj.collision);
        }

        return 1;
    }
}

/**
 *   Common part of a cown that is independent of the data the cown is storing.
 *
 *   Just contains a pointer to the last behaviour's request for this cown.
 */
class CownBase : StableOrder
{
    // Points to the end of the queue of requests for this cown.
    // If it is null, then the cown is not currently in use by 
    // any behaviour.
    volatile internal Request? last = null;
}

class Behaviour
{
    // The body of the behaviour.
    Action thunk;
    // How many requests are outstanding for the behaviour.
    int count;
    // The set of requests for this behaviour.
    // This is used to release the cowns to the subseqeuent behaviours.
    Request[] requests;

    internal Behaviour(Action t, Request[] r)
    {
        thunk = t;
        requests = r;
        // We add an additional count, so that the 2PL is finished
        // before we start running the thunk.
        // Note: this is probably not required in a GCed language,
        // but the C++ version definitely requires it.
        count = r.Count() + 1;
        Terminator.Increment();
    }

    // Called when a request is at the head of the queue for a particular cown.
    internal void resolve_one()
    {
        if (Interlocked.Decrement(ref count) != 0)
            return;

        // Last request so schedule the task.
        Task.Run(() =>
            {
                // Run body.
                thunk();
                // Release all the cowns.
                foreach (var r in requests)
                {
                    r.Release();
                }
                Terminator.Decrement();
            }
        );
    }
}

class Request
{
// Disable warning, these objects (wait,last) are used as special values, and 
// their fields are never inspected.
#pragma warning disable CS8625    
    // Special request to represent that this is part way through an enqueue
    // operation and subsequent requests should wait to obey the 2PL.
    static Request WAIT = new Request(null, null);
    // Used to represent that this the 2PL is complete, and subsequent request
    // can be enqueued.
    static Request READY = new Request(null, null);
    // Pointer to the next request in the queue. May take the special values of
    // wait and last if the next value is not yet known.
#pragma warning restore CS8625
    volatile Request next = WAIT;

    // The cown that this request is for.
    CownBase target;

    // The behaviour that this request is for.
    Behaviour behaviour;

    // This a local state to connect the two phases of the 2PL together.
    // Could use `next` but it would complicate the understanding of the code.
    Request? prev = null;

    public Request(Behaviour b, CownBase t)
    {
        behaviour = b;
        target = t;
    }

    internal void Release()
    {
        // This code is effectively a MCS-style queue lock release.
        if (next == READY)
        {
            if (Interlocked.CompareExchange<Request?>(ref target.last, null, this) == this)
            {
                return;
            }

            // Spin waiting for this to be set to something else.
            while (next == READY) { }
        }
        next.behaviour.resolve_one();
    }

    internal void StartEnqueue()
    {
        prev = Interlocked.Exchange<Request?>(ref target.last, this);

        if (prev == null)
        {
            behaviour.resolve_one();
            return;
        }

        // Spin wait here.
        while (prev.next != READY) { }
    }

    internal void FinishEnqueue()
    {
        if (prev != null)
            prev.next = this;

        next = READY;

        // Needed otherwise GC will never collect any requests as they will all be in a linked list.
        prev = null;
    }
}

class Cown<T> : CownBase
{

    internal T value;

    public Cown(T v) { value = v; }
}

class When
{
    private static void schedule(Action thunk, params CownBase[] cowns)
    {
        Array.Sort(cowns);
        var requests = new Request[cowns.Length];
        var b = new Behaviour(thunk, requests);
        for (int i = 0; i < cowns.Length; i++)
        {
            requests[i] = new Request(b, cowns[i]);
        }

        // Complete first phase of 2PL enqueuing on all cowns.
        for (int i = 0; i < cowns.Length; i++)
        {
            requests[i].StartEnqueue();
        }

        // Complete second phase of 2PL enqueuing on all cowns.
        for (int i = 0; i < cowns.Length; i++)
        {
            requests[i].FinishEnqueue();
        }

        // Resolve the additional request.
        b.resolve_one();
    }

    public static Action<Action> when()
    {
        return f => schedule(() => { f(); });
    }

    public static Action<Action<T>> when<T>(Cown<T> t)
    {
        return f =>
        {
            var thunk = () => f(t.value);
            schedule(thunk, t);
        };
    }

    public static Action<Action<T1, T2>> when<T1, T2>(Cown<T1> t1, Cown<T2> t2)
    {
        return (f) =>
        {
            var thunk = () => f(t1.value, t2.value);
            schedule(thunk, t1, t2);
        };
    }

    public static Action<Action<T1, T2, T3>> when<T1, T2, T3>(Cown<T1> t1, Cown<T2> t2, Cown<T3> t3)
    {
        return (f) =>
        {
            var thunk = () => f(t1.value, t2.value, t3.value);
            schedule(thunk, t1, t2, t3);
        };
    }
}

// This class detects termination of the program.
// Effectively just a reference count, and a way to wait for it to
// reach 0.
class Terminator
{
    private static volatile int count = 1;
    private static ManualResetEvent mre = new ManualResetEvent(false);

    public static void Wait()
    {
        Decrement();
        mre.WaitOne();
    }

    public static void Increment()
    {
        Interlocked.Increment(ref count);
    }

    public static void Decrement()
    {
        if (Interlocked.Decrement(ref count) == 0)
        {
            mre.Set();
        }
    }
}