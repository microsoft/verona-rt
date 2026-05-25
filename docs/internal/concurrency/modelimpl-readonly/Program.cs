using static When;

var c1 = new Cown<int>(1);
var c2 = new Cown<int>(2);

when()(
    () => Console.WriteLine("when without cowns")
);

when(c1)(x =>
{
    Thread.Sleep(1);
    Console.WriteLine("when on cown:" + x);
    when(c2)(
        x => Console.WriteLine("nested when on cown:" + x)
    );
});


when(c2)(x =>
{
    Console.WriteLine("when on cown:" + x);
    when(c1)(
        x => Console.WriteLine("nested when on cown:" + x)
    );
});

when(c1, c2)((x1, x2) =>
    Console.WriteLine("when on cown:" + x1 + " and " + x2)
);

when(c2)(x =>
{
    Console.WriteLine("when on cown:" + x);
});

// -----------------------------------------------------------------------------
//  Read-only cown scenarios.
// -----------------------------------------------------------------------------

// Solo read.
when(read(c1))(x => Console.WriteLine("read solo on c1:" + x));

// W -> R -> R -> W cascade on c1.
when(c1)(x => Console.WriteLine("write c1:" + x));
when(read(c1))(x => Console.WriteLine("read#1 c1:" + x));
when(read(c1))(x => Console.WriteLine("read#2 c1:" + x));
when(c1)(x => Console.WriteLine("write after reads c1:" + x));

// Two concurrent reads should be permitted to run simultaneously.
when(read(c2))(x => { Thread.Sleep(5); Console.WriteLine("read A c2:" + x); });
when(read(c2))(x => { Thread.Sleep(5); Console.WriteLine("read B c2:" + x); });

// Duplicate cowns.
when(c1, c1)((x, y) => Console.WriteLine("when(c1,c1):" + x + "," + y));
when(c2, read(c2))((x, y) => Console.WriteLine("when(c2, read(c2)):" + x + "," + y));
when(read(c1), read(c1))((x, y) => Console.WriteLine("when(read(c1), read(c1)):" + x + "," + y));

// Mixed-arity with reads.
when(read(c1), c2)((x, y) =>
    Console.WriteLine("read c1 + write c2: " + x + "," + y));

// -----------------------------------------------------------------------------
//  Deterministic cascade-walker scenarios. These pin enqueue order with
//  ManualResetEventSlim so the writer's WakeupReaderChain MUST walk past
//  multiple slots in a single Release. Without these scenarios the chain
//  walker (When.cs WakeupReaderChain) is only exercised by very specific
//  timings that may not arise under nominal scheduling.
// -----------------------------------------------------------------------------

{
    // (a) writer-then-many-readers-then-writer. The writer's body parks
    // itself on a manual gate; all readers + the trailing writer enqueue
    // behind it. When the gate releases, the cascade walker walks past
    // every reader to the trailing writer (writerAtEnd = true,
    // extraReaders >= 4).
    var cA = new Cown<int>(0);
    var gate = new ManualResetEventSlim(false);
    var readersEnqueued = new CountdownEvent(5);

    when(cA)(x =>
    {
        gate.Wait();
        Console.WriteLine($"H5(a) writer body: {x}");
    });
    for (int i = 0; i < 5; i++)
    {
        int idx = i;
        when(read(cA))(x =>
        {
            Console.WriteLine($"H5(a) reader#{idx}: {x}");
            readersEnqueued.Signal();
        });
    }
    when(cA)(x => Console.WriteLine($"H5(a) trailing writer: {x}"));

    // Release the gate from a background task so the writer body can
    // complete; the cascade walker then runs.
    _ = Task.Run(() =>
    {
        Thread.Sleep(50); // let readers + trailing writer all enqueue
        gate.Set();
    });
}

{
    // (b) writer -> R -> R -> W (writerAtEnd = true, extraReaders = 1).
    var cB = new Cown<int>(0);
    var gate = new ManualResetEventSlim(false);

    when(cB)(x =>
    {
        gate.Wait();
        Console.WriteLine($"H5(b) writer body: {x}");
    });
    when(read(cB))(x => Console.WriteLine($"H5(b) reader#0: {x}"));
    when(read(cB))(x => Console.WriteLine($"H5(b) reader#1: {x}"));
    when(cB)(x => Console.WriteLine($"H5(b) trailing writer: {x}"));

    _ = Task.Run(() => { Thread.Sleep(50); gate.Set(); });
}

{
    // (c) Parked writer arriving AFTER chain opens. Open a read chain on
    // cC, then schedule a writer; the writer takes the
    // try_write-then-park path (cown.nextWriter rendezvous). The last
    // reader's DropRead must observe the parked writer and wake it via
    // WakeupNextWriter.
    var cC = new Cown<int>(0);
    var readerGate = new ManualResetEventSlim(false);
    when(read(cC))(x =>
    {
        readerGate.Wait();
        Console.WriteLine($"H5(c) reader: {x}");
    });
    when(cC)(x => Console.WriteLine($"H5(c) parked writer woken: {x}"));
    _ = Task.Run(() => { Thread.Sleep(50); readerGate.Set(); });
}

// Wait for the when runtime to finish all the work.
Terminator.Wait();
