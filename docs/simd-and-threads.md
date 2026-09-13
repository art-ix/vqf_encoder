# SIMD and parallel VQ search

The encoder supports `--simd auto|scalar|sse41|avx2` and `--threads auto|1..32`.
The C++ equivalents are `Encoder::Config::simd` (`Encoder::Simd`) and
`Encoder::Config::threads`. Defaults are automatic SIMD and automatic workers (`threads = 0` in C++).
For example:

```sh
vqf_encode -b 96 --ppc-search --threads 4 --simd auto input.wav output.vqf
```

## Parallel work

Frames remain sequential: LSP/Bark histories, overlap and optional temporal
error feedback must come from the selected preceding frame. Within main VQ,
each group reads immutable targets, weights, codebooks and permutation data,
and writes exactly two independent coefficient bytes. Partition those groups
into contiguous chunks of up to eight vectors, claimed by available workers
through an atomic cursor. Each executing thread reuses its own
target/weight/residual buffers across chunks and frames. Storage grows only
when a larger codebook vector is needed; active elements are overwritten before
use, so it carries no quantizer history between Encoders. Worker scratch storage
is released when its thread exits; caller scratch storage lasts until the caller
thread exits.
Join all workers before dequantization, gain fitting or candidate selection.

The caller also claims chunks alongside a reusable worker pool created lazily
for the first parallel search. Dynamic assignment reduces waiting when some
vectors need more distance evaluations or pruning is less effective. Its capacity grows only when
a later block needs more workers; inactive workers sleep between batches.
The effective worker count remains bounded by at least 16 groups per worker;
small PPC shape searches stay on the caller. All batch work finishes before
returning, including when a worker or caller throws. A failed batch does not
poison subsequent batches. Flush releases this Encoder's pool reference, and
pool destruction stops and joins its threads.

Copying an Encoder preserves its state and may share an existing pool; the
pool serializes concurrent batches from those copies. Independently constructed
Encoders own independent pools. The Encoder API still requires sequential
feed/flush calls on each instance; frames are not encoded in parallel.

More threads cost CPU resources and may not improve small inputs or simultaneous
multi-file encoding. Automatic selection uses `std::thread::hardware_concurrency()`, limited to eight
logical CPUs (one if the count is unknown), and the group limit described above.
Thus 2/4/8 reported logical CPUs allow up to 2/4/8 workers. This portable API
reports hardware threads, not physical cores, and may not reflect every host CPU
quota. Explicit `--threads N` overrides the automatic cap, while `--threads 1`
disables parallel work. The CLI reports the resolved upper bound. The Makefile adds `-pthread`; Windows uses the C++
standard library threading implementation, without an OpenMP dependency.

## SIMD work

SSE4.1 evaluates four weighted distance terms at a time. AVX2 beam seeding
uses eight terms at a time, retaining scalar accumulation and the four-term
cutoff. AVX2 codebook scans instead evaluate eight candidates simultaneously:
each lane accumulates one candidate's error in the original bin order.
Candidate selection still follows the original index/sign order with strict
ties. A shared cutoff can reject the whole group after four-bin checkpoints;
it may do extra work compared with updating the cutoff after every candidate.

For those scans, the caller prepares transposed float codebooks containing
positive entries and interleaved positive/negative entries. Each caller thread
caches immutable tables by source address and vector stride, preparing them
before publishing worker tasks. Tables are retained until that thread exits.
The original int16 tables remain in use for reconstruction and other kernels.
Main VQ and PPC use this scan; scalar/SSE4.1 retain their existing paths.

MSVC locally overrides the library’s `/fp:fast` setting with precise semantics
and disabled contraction for these kernels. No horizontal reduction, fused
multiply-add, reassociation, wider beam changes or altered quantizer decisions
are introduced. Padded final candidate groups use a valid-lane mask; target and
weight loads stay within the requested vector length.

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
thresholds including ties. The candidate-lane kernel is also checked against
scalar codebook scans with duplicate entries, zero weights, signs, unaligned
source addresses, partial candidate groups and vector tails. Unsupported CPU
kernels are skipped.

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

MSVC floating-point control follows [Microsoft's float_control documentation](https://learn.microsoft.com/en-us/cpp/preprocessor/float-control).
The option test also compares explicit and implicit automatic worker selection.

`--test-workers` checks fixed partitions and dynamic chunks (including tails,
fewer chunks than workers, and exactly-once coverage), changing worker counts,
uneven/empty ranges, concurrent
batch callers, exception propagation, reuse after failure and continued encoding after copying
an Encoder and flushing one owner. It is run by the
parallel options script, including in Windows CI. Encoder equivalence checks
verify that the pool changes execution scheduling, not encoded decisions.
