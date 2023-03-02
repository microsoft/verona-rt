// This class detects termination of the program.
// Effectively just a reference count, and a way to wait for it to
// reach 0.
class Terminator
{
    private static volatile int count = 1;
    private static ManualResetEvent mre = new ManualResetEvent(false);

    // Wait for the reference count to reach 0.
    // Should be called at most once.
    public static void Wait()
    {
        Decrement();
        mre.WaitOne();
    }

    // Increment the reference count.
    public static void Increment()
    {
        Interlocked.Increment(ref count);
    }

    // Decrement the reference count. If it reaches 0, then signal the
    // Waiting thread.
    public static void Decrement()
    {
        if (Interlocked.Decrement(ref count) == 0)
        {
            mre.Set();
        }
    }
}