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

/**
 *  Behaviour that captures the content of a when body.
 *
 *  It contains all the state required to run the body, and release the
 *  cowns when the body has finished.
 */
class Behaviour
{
    // The body of the behaviour.
    Action thunk;
    // How many requests are outstanding for the behaviour.
    int count;
    // The set of requests for this behaviour.
    // This is used to release the cowns to the subseqeuent behaviours.
    Request[] requests;

    // Creates and schedules a new Behaviour.
    internal Behaviour(Action t, params CownBase[] cowns)
    {
        thunk = t;
        // We add an additional count, so that the 2PL is finished
        // before we start running the thunk. Without this, the calls to
        // Release at the end of the thunk could race with the calls to 
        // FinishEnqueue in the 2PL.
        count = cowns.Count() + 1;

        Array.Sort(cowns);
        requests = new Request[cowns.Length];
        for (int i = 0; i < cowns.Length; i++)
        {
            requests[i] = new Request(cowns[i]);
        }
    }

    // Schedule this behaviour
    // Performs two phase locking (2PL) over the enqueuing of the requests.
    // This ensures that the overall effect of the enqueue is atomic.
    internal void Schedule()
    {
        // Complete first phase of 2PL enqueuing on all cowns.
        foreach (var r in requests)
        {
            r.StartEnqueue(this);
        }

        // Complete second phase of 2PL enqueuing on all cowns.
        foreach (var r in requests)
        {
            r.FinishEnqueue();
        }

        // Resolve the additional request. [See comment in the Constructor]
        // All the cowns may already be resolved, in which case, this will
        // schedule the task.
        resolve_one();

        // Prevent runtime exiting until this has run.
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
    // Pointer to the next behaviour in the queue.
    volatile Behaviour? next = null;

    // Flag to indicate the associated behaviour to this request has been
    // scheduled
    volatile bool scheduled = false;

    // The cown that this request is for.
    CownBase target;

    public Request(CownBase t)
    {
        target = t;
    }

    internal void Release()
    {
        // This code is effectively a MCS-style queue lock release.
        if (next == null)
        {
            if (Interlocked.CompareExchange<Request?>(ref target.last, null, this) == this)
            {
                return;
            }

            // Spin waiting for this to be set to something else.
            while (next == null) { }
        }
        next.resolve_one();
    }

    /**
     *   Start the first phase of the 2PL enqueue operation.
     *
     *   This enqueues the request onto the cown.  It will only return
     *   once any previous behaviour on this cown has finished enqueueing
     *   on all its required cowns.  This ensures that the 2PL is obeyed.
     */
    internal void StartEnqueue(Behaviour behaviour)
    {
        var prev = Interlocked.Exchange<Request?>(ref target.last, this);

        if (prev == null)
        {
            behaviour.resolve_one();
            return;
        }

        prev.next = behaviour;

        // Spin wait here.
        while (!prev.scheduled) { }
    }

    /**
     *  Finish the second phase of the 2PL enqueue operation.
     *
     *  This will set the scheduled flag, so subsequent behaviours on this
     *  cown can continue the 2PL enqueue.
     */
    internal void FinishEnqueue()
    {
        scheduled = true;
    }
}

class Cown<T> : CownBase
{
    internal T value;

    public Cown(T v) { value = v; }
}

/**
 * This class provides the when() function for various arities.
 */
class When
{
    public static Action<Action> when()
    {
        return f => new Behaviour(f).Schedule();
    }

    public static Action<Action<T>> when<T>(Cown<T> t)
    {
        return f =>
        {
            var thunk = () => f(t.value);
            new Behaviour(thunk, t).Schedule();
        };
    }

    public static Action<Action<T1, T2>> when<T1, T2>(Cown<T1> t1, Cown<T2> t2)
    {
        return (f) =>
        {
            var thunk = () => f(t1.value, t2.value);
            new Behaviour(thunk, t1, t2).Schedule();
        };
    }

    public static Action<Action<T1, T2, T3>> when<T1, T2, T3>(Cown<T1> t1, Cown<T2> t2, Cown<T3> t3)
    {
        return (f) =>
        {
            var thunk = () => f(t1.value, t2.value, t3.value);
            new Behaviour(thunk, t1, t2, t3).Schedule();
        };
    }
}
