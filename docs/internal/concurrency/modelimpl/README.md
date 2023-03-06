# Model implementation of the Verona Concurrency Model

This directory contains a C# implementation of the Verona concurrency model.
This implementation is designed primarily for understanding how to implement the model.
But can also be used to experiment with the concurrency model in a different context.

The C# implementation is built on top of the .NET task implementation.
This means it can focus on the aspects unique to Behaviour Oriented Concurrency. 

The C++ implementation avoids allocations by consolidating multiple objects into a single allocation.
This improves efficiency but is less readable.
This is not possible in the .NET implementation as interior pointers cannot be used on the heap.
