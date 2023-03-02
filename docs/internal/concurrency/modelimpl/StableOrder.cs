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
