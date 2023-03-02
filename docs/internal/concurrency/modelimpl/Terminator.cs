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