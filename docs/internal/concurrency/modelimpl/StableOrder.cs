 /// <summary>
 ///   Used to provide a stable order over object.
 /// </summary>
 /// <remarks>
 /// Simple implementation that uses an atomic global counter at construction time.
 /// </remarks>
class StableOrder : IComparable
{
    /// <summary>
    ///     Source of unique identities.
    /// </summary>
    /// <remarks>
    ///  Assuming not wrapping around a 64bit counter.
    /// </remarks>
    private static volatile int counter = 0;

    /// <summary>The identity of this object.</summary>
    private Int64 identity = 0;

    protected StableOrder()
    {
        identity = Interlocked.Increment(ref counter);
    }

    public int CompareTo(object? obj)
    {
        if (obj is not null && obj is StableOrder sobj)
        {
            return identity.CompareTo(sobj.identity);
        }

        return 1;
    }
}
