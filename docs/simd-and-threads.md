# SIMD and parallel VQ search

The encoder supports `--simd auto|scalar|sse41|avx2` and `--threads 1..32`.
The C++ equivalents are `Encoder::Config::simd` (`Encoder::Simd`) and
`Encoder::Config::threads`. Defaults are automatic SIMD and one worker.
For example:

```sh
vqf_encode -b 96 --ppc-search --threads 4 --simd auto input.wav output.vqf
```

## Parallel work

Frames remain sequential: LSP/Bark histories, overlap and optional temporal
error feedback must come from the selected preceding frame. Within main VQ,
each group reads immutable targets, weights, codebooks and permutation data,
and writes exactly two independent coefficient bytes. Partition those groups
into contiguous ranges; each worker owns its target/weight/residual buffers.
Join all workers before dequantization, gain fitting or candidate selection.

One range runs on the caller; up to `threads - 1` ranges use `std::async` with
`std::launch::async`. Tasks are joined even when an exception unwinds the call.
The effective worker count is bounded by at least 16 groups per worker; small
PPC shape searches stay on one worker. There is no persistent thread pool or
parallel frame encoding. The Encoder API still requires sequential feed/flush
calls on a given instance. Separate Encoder instances can encode separate files.

More threads cost CPU resources and may not improve small inputs or simultaneous
multi-file encoding. The option remains explicit rather than consuming every
available CPU by default. The Makefile adds `-pthread`; Windows uses the C++
standard library threading implementation, without an OpenMP dependency.

## SIMD work

SSE4.1 evaluates four weighted distance terms at a time; AVX2 evaluates eight.
Both retain scalar accumulation in the original order and the original cutoff
after every four terms. AVX2 can compute four extra terms before an early exit.
No horizontal reduction, fused multiply-add, reassociation, wider beam changes
or altered quantizer decisions are introduced. Loads never cross the vector
length, including short tails and unaligned codebook addresses.

Automatic dispatch prefers AVX2, then SSE4.1, then scalar. GCC/Clang use CPU
feature built-ins and per-function targets. MSVC checks CPUID plus OS-enabled
XMM/YMM state before AVX2. SIMD functions are kept separate from the baseline
path; do not enable AVX2 globally for a portable binary. Explicit unsupported
backends produce an error. Non-x86 builds retain the scalar path.

Reference API details: [GCC x86 feature built-ins](https://gcc.gnu.org/onlinedocs/gcc/x86-Built-in-Functions.html)
and [Microsoft x64 intrinsics](https://learn.microsoft.com/en-us/cpp/intrinsics/x64-amd64-intrinsics-list).

## Checks and limits

`--test-simd` compares scalar/SSE4.1/AVX2 errors bit for bit for both signs,
int16 extremes, zero weights, misaligned starts, lengths 1..65 and cutoff
thresholds including ties. Unsupported CPU kernels are skipped.

`python tools/test_parallel_options.py ENCODER [PREVIOUS_ENCODER]` compares
complete bitstreams across available SIMD backends and one/four workers, plus
auto selection. Synthetic fixtures exercise Long/PPC, adaptive/masking/temporal,
Medium and mono modes. Supplying the previous binary checks pre-change output
as well. `--test-codec-parallel` exercises all legal mono/stereo modes with four
workers, irregular feed sizes, flush, silence and existing decoder quality gates.
Both suites are included in Windows CI and `make test`.

Equivalence checks concern the same platform with the default floating-point
environment and ordinary precise build options. Do not use fast-math or global
FMA contraction to infer equivalent output. Timings depend on CPU, available
cores and workload; SIMD width alone does not predict an additional speedup.
