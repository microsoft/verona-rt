/// <summary>
///   Common part of a cown that is independent of the data the cown is storing.
/// </summary>
/// <remarks>
///  Just contains a pointer to the last behaviour's request for this cown.
/// </remarks>
class CownBase : StableOrder
{
    /// <summary>
    ///   Points to the end of the queue of requests for this cown.
    /// </summary>
    /// <remarks>
    ///  If it is null, then the cown is not currently in use by
    ///  any behaviour.
    /// </remarks>
    volatile internal Request? last = null;
}

/// <summary>
///   Behaviour that cpatures the content of a when body.
/// </summary>
/// <remarks>
///   It contains all the state required to run the body, and release the
///   cowns when the body has finished.
/// </remarks>
class Behaviour
{
    /// <summary>
    ///   The body of the behaviour.
    /// </summary>
    Action thunk;

    /// <summary>
    ///   How many requests are outstanding for the behaviour.
    /// </summary>
    int count;
    
    /// <summary>
    ///   The set of requests for this behaviour.
    /// </summary>
    /// <remarks>
    ///  This is used to release the cowns to the subseqeuent behaviours.
    /// </remarks>
    Request[] requests;

    /// <summary>
    ///  Creates and schedules a new Behaviour.
    /// </summary>
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

    /// <summary>
    ///   Schedules the behaviour.
    /// </summary>
    /// <remarks>
    ///  Performs two phase locking (2PL) over the enqueuing of the requests.
    ///  This ensures that the overall effect of the enqueue is atomic.
    /// </remarks>
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
        ResolveOne();

        // Prevent runtime exiting until this has run.
        Terminator.Increment();
    }

    /// <summary>
    ///   Resolves a single outstanding request for this behaviour.
    /// </summary>
    /// <remarks>
    ///  Called when a request is at the head of the queue for a particular cown.
    ///  If this is the last request, then the thunk is scheduled.
    /// </remarks>
    internal void ResolveOne()
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

/// <summary>
///   A request for a cown.
/// </summary>
/// <remarks>
///   This is used to wait for a cown to be available for a particular behaviour.
/// </remarks>
class Request
{
    /// <summary>Pointer to the next behaviour in the queue.</summary>
    volatile Behaviour? next = null;

    /// <summary>
    ///  Flag to indicate the associated behaviour to this request has been
    ///  scheduled
    /// </summary>
    volatile bool scheduled = false;

    /// <summary>
    ///   The cown that this request is for.
    /// </summary>
    CownBase target;

    public Request(CownBase t)
    {
        target = t;
    }

    /// <summary>
    ///   Release the cown to the next behaviour.
    /// </summary>
    /// <remarks>
    ///  This is called when the associated behaviour has completed, and thus can 
    ///  allow any waiting behaviour to run.
    /// 
    ///  If there is no next behaviour, then the cown's `last` pointer is set to null.
    /// </remarks>
    internal void Release()
    {
        // This code is effectively a MCS-style queue lock release.
        if (next == null)
        {
            if (Interlocked.CompareExchange<Request?>(ref target.last, null, this) == this)
            {
                return;
            }

            // Wait for the next pointer to be set. The target.last is no longer us
            // so this should not take long.
            var w = new SpinWait();
            while (next == null) { w.SpinOnce(); }
        }
        next.ResolveOne();
    }

    /// <summary>
    ///  Start the first phase of the 2PL enqueue operation.
    /// </summary>
    /// <remarks>
    ///  This enqueues the request onto the cown.  It will only return
    ///  once any previous behaviour on this cown has finished enqueueing
    ///  on all its required cowns.  This ensures that the 2PL is obeyed.
    /// </remarks>
    internal void StartEnqueue(Behaviour behaviour)
    {
        var prev = Interlocked.Exchange<Request?>(ref target.last, this);

        if (prev == null)
        {
            behaviour.ResolveOne();
            return;
        }

        prev.next = behaviour;

        var w = new SpinWait();
        while (!prev.scheduled) { w.SpinOnce(); }
    }

    /// <summary>
    ///   Finish the second phase of the 2PL enqueue operation.
    /// </summary>
    /// <remarks>
    ///   This will set the scheduled flag, so subsequent behaviours on this
    ///   cown can continue the 2PL enqueue.
    /// </remarks>
    internal void FinishEnqueue()
    {
        scheduled = true;
    }
}

/// <summary>
///   Cown that wraps a value.  The value should only be accessed inside
///   a when() block.
/// </summary>
/// <typeparam name="T">The type that is wrapped by the cown.</typeparam>
class Cown<T> : CownBase
{
    internal T value;

    public Cown(T v) { value = v; }
}

/// <summary>
///   This class proviated the when() function for various arities.
/// </summary>
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
