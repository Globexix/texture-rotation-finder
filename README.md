# texture-rotation-finder

Minecraft coordinate finder using block texture rotations, written in Fas.

It searches for world coordinates that match a set of observed texture rotations.

## Algorithm

Possible Y values for each X/Z position are packed into a `u64`.

The fast path generates 64-Y rotation signatures only on sparse Z rows, then applies observations with bitwise ANDs instead of checking every Y separately.

Source signatures are generated using a finite-difference recurrence, and mirrored `(x, z)` signatures are reused when possible.

The current sparse cover uses:

```text
z mod 32 = ±2, ±8, ±13
```

Candidates that survive the mask pass are verified with the exact rotation function.

## Benchmark

Search area:
x: -225000 .. 225000
y: -60 .. 0
z: -225000 .. 225000

AMD Ryzen 9 9950X, 32 threads, `-O3 -march=native`.

| Threads |      Mean |
| ------- | --------: |
| 32      |  78.96 s |
| 1       |22m 37s|

Sin

Measured with Hyperfine, 1 warmup and 3 runs.

## Current limits

The optimized path currently targets:

```text
Y = -60..0
observation dy = 0 or 1
```

There is a generic exact worker for arbitrary Y ranges and 3D patterns, but it does not use the optimized mask algorithm yet.
