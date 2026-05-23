 /// <summary>
 ///   Used to provide a stable order over object.
 /// </summary>
 /// <remarks>
 /// Simple implementation that uses an atomic global counter at construction time.
 /// </remarks>
class StableOrder : IComparable, IComparable<StableOrder>
{
    /// <summary>
    ///     Source of unique identities.
    /// </summary>
    /// <remarks>
    ///   64-bit counter so the identity space does not wrap during the
    ///   process lifetime. `Interlocked.Increment` has a `long` overload;
    ///   `volatile` does not apply to `long` and is omitted.
    ///   Shared via &lt;Compile Link&gt; with the modelimpl-readonly
    ///   sibling; any change to identity allocation must keep values
    ///   monotonically distinct across both models for the lifetime of
    ///   the process.
    /// </remarks>
    private static long counter = 0;

    /// <summary>The identity of this object.</summary>
    private Int64 identity = 0;

    protected StableOrder()
    {
        identity = Interlocked.Increment(ref counter);
    }

    public int CompareTo(StableOrder? other)
    {
        if (other is null) return 1;
        return identity.CompareTo(other.identity);
    }

    public int CompareTo(object? obj)
    {
        if (obj is StableOrder sobj) return CompareTo(sobj);
        return 1;
    }
}
