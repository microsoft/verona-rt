/**
 *  Used to provide a stable order over object.
 * 
 *  Simple implementation that uses an atomic global counter at construction time.
 **/
class StableOrder : IComparable
{
    // Source of unique identities. (Assuming not wrapping around a 64bit counter.)
    private static volatile int counter = 0;

    // The identity of this object.
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
