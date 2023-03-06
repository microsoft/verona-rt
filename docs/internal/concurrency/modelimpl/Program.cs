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

// Wait for the when runtime to finish all the work.
Terminator.Wait();