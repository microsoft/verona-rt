/// <summary>
///  Detects termination of the program.
/// </summary>
/// <remarks>
///  Effectively just a reference count, and a way to wait for it to
///  reach 0.
/// </remarks>
class Terminator
{
    private static volatile int count = 1;
    private static ManualResetEvent mre = new ManualResetEvent(false);

    /// <summary>
    ///  Wait for the reference count to reach 0.
    /// </summary>
    /// <remarks>
    ///  Should be called at most once.
    /// </remarks>
    public static void Wait()
    {
        Decrement();
        mre.WaitOne();
    }

    /// <summary>Increment the reference count.</summary>
    public static void Increment()
    {
        Interlocked.Increment(ref count);
    }

    /// <summary>Decrement the reference count.</summary>
    /// <remarks>
    ///  If it reaches 0, then signal the Waiting thread.
    /// </remarks>
    public static void Decrement()
    {
        if (Interlocked.Decrement(ref count) == 0)
        {
            mre.Set();
        }
    }
}