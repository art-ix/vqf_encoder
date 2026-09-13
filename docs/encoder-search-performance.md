# Encoder search performance

The optimized encoder retains the existing beam sizes, PPC proposals, gain
fits, masking weights and block decisions. No faster/lower-quality preset or
new runtime option is introduced.

## Work removed

- **Bounded VQ distance:** squared weighted error has nonnegative terms.
  After each group of four bins, stop scoring a candidate whose accumulated
  error is already at least the incumbent error (or the last beam entry).
  It cannot enter the selected set. Keep the original summation order and
  strict comparison rules, including ties; do not reorder bins or enable
  relaxed floating-point reassociation.
- **Duplicate prepared frames:** after LSP/PPC/Bark and initial gain fitting,
  compare all transmitted envelope fields. Different search routes that have
  identical fields and the same incoming history need only one main-VQ search.
  The cache is local to a frame and is cleared before temporal winner replay.
- **Unchanged first gain fit:** reuse the first main-VQ result when fitting
  the decoded gain did not change it. Target and weights are then unchanged.
- **Decoded gain table:** initialize the Long-frame gain search table once
  using the same float expressions instead of evaluating exponentials on
  every candidate fit. C++ local-static initialization is thread safe.

## Grouped codebook searches

Pair refinement and forward/reverse pair searches dispatch once per codebook
scan, instead of making an indirect SIMD call for every candidate and sign.
The SSE4.1 and AVX2 scans inline their ordered distance kernel inside the
ISA-specific function; the scalar path implements the same interface.
Candidates still run in their original order, with the incumbent error updated
immediately. This preserves four-bin pruning, strict ties and the original
floating-point summation. SIMD lanes continue to represent frequency bins;
this change does not evaluate multiple candidates simultaneously.

`--test-simd` checks grouped winners and errors against candidate-at-a-time
scalar scans, including signs, duplicate entries, unaligned input, partial SIMD
groups, and finite cutoffs that may reject every candidate. The existing
encoder equivalence script checks complete output against a pre-change binary.

## AVX2 across candidates

For pair searches and coordinate refinement, AVX2 lanes now represent eight
candidates rather than eight bins. Codebooks are transposed once per source and
stride on each caller thread, with separate unsigned and interleaved signed
layouts. Each lane preserves the old ordered weighted-error accumulation.
Selection proceeds in original candidate order after each group. Whole-group
pruning uses the incumbent at group entry, which is a safe but sometimes looser
cutoff than the previous per-candidate scan. This trades some extra arithmetic
for parallel accumulators and fewer candidate-loop iterations.

Beam seeding retains its existing kernel. The runtime SIMD selection and the
scalar/SSE4.1 fallbacks are unchanged. The extended SIMD test, encoder byte
comparison and full codec/parallel suites cover the new path.

## Grouped beam seeding

Forward and reverse beam seeding now also dispatch once per codebook. The
ISA-specific loop scores candidates and inserts them in the original stable
order. A candidate whose bounded error is already at least the last beam error
is rejected immediately, avoiding an unsuccessful walk through every beam
slot. The beam widths, candidate order, distance summation, pruning thresholds
and strict tie rules remain unchanged. Grouping alone did not establish a
consistent timing benefit in the limited checks; the retained variant includes
the early rejection before insertion.

`--test-simd` compares the resulting beam against a stable sort of unpruned
scalar distances for widths 4/8/16/32. Coverage includes duplicate codebook
entries, zero-weight ties, signs, vector tails, unaligned inputs, fewer
candidates than beam slots and output-boundary sentinels.

## Converged pair refinement

After a complete coordinate-refinement pass (both codebooks), stop if the
incumbent error did not decrease. Pair indices and signs update only on strict
improvement, so an unchanged error means the next pass would repeat the same
inputs, comparisons and result. The check uses the exact float value, with no
epsilon or approximate convergence threshold. Forward/reverse beam candidates
and prefix refinement calls are still evaluated as before; only a redundant
second pass is removed. This applies to both main VQ and PPC shape VQ.

A vector now also remembers its most recently confirmed fixed point: both
codebook indices, both signs and the exact incumbent error. If a later beam
prefix calls refinement with that identical state, it reuses the convergence
result. A refinement that reaches its pass limit while still improving is not
marked converged. The record is local to one target/weight vector, and every
beam candidate is still evaluated before deciding whether refinement is needed.

## Reusing coordinate search bounds

Each vector keeps one most recent scan record for each coordinate direction:
the fixed codeword index, its sign and the error bound established by the scan.
For identical fixed inputs, no candidate can beat that bound. If a later
refinement uses the same codeword/sign with an equal or smaller incumbent
error, it can skip rebuilding the residual and rescanning the codebook.
A larger incumbent triggers a fresh scan, including after reverse-search
initialization when needed. The records do not cross vector or frame boundaries.

This is two small records, not a table of codebook-pair distances. The two
directions stay separate to preserve their floating-point subtraction order.
All comparisons are exact; beam widths, candidates and strict tie handling
remain unchanged. The same rule covers main VQ and PPC shape refinement.

## Work chunk sizing experiment (not retained)

An alternative grain size, `clamp(vector_count / (workers * 8), 2, 16)`, was
compared with the retained fixed grain of eight, using two and eight workers.
Although output remained identical, the limited checks showed regressions with
two workers and mixed gains elsewhere. The automatic grain formula is not
present in the retained encoder; automatic worker-count selection is unchanged.

## Reusable VQ scratch buffers

The three temporary float vectors used by each range are now thread-local.
This avoids allocating, zero-initializing and freeing all three for every
eight-vector chunk. Buffers grow when necessary and retain their size; only
active elements participate in the search, and those are initialized before
use. Different worker threads never share scratch buffers. Sequential calls
from different Encoders on the same thread can reuse storage without retaining
search decisions or depending on the previous vector length.

## Validation and reproduction

Build a reference binary before the optimization and the candidate with the
same compiler and floating-point options. Run:

```sh
python tools/test_encoder_equivalence.py bin/vqf_encode /path/to/reference/vqf_encode
bin/vqf_encode --test-codec-ppc
bin/vqf_encode --test-codec-ppc-time
```

The equivalence script generates synthetic PCM in a temporary directory and
compares complete VQF bytes. Its four cases cover mono with LSP/Bark searches
disabled, default stereo, Medium blocks with masking, and adaptive blocks with
masking/PPC/temporal ranking; beam sizes 4, 8, 16 (auto) and 32 are exercised.
It prints single-run timings as a local smoke benchmark, not a portable speed
guarantee. The codec suites cover all legal modes in mono/stereo, chunking,
flush, silence, duration and existing quality/pre-echo gates.

Byte equality is tested on the same build platform. It is not a promise of
identical output across compilers or floating-point architectures. Windows CI
also exercises the codec regressions.

## Additional cache experiments (not retained)

Two further memoization approaches were evaluated against the retained encoder.
Both preserved complete output bytes in the limited equivalence checks, but
neither established a useful overall speed improvement:

- Per-frame LSP results indexed by channel/search strategy, plus PPC results
  indexed by transmitted LSP parameters, removed repeated work across Bark/PPC
  trials. End-to-end timing gains were small or absent across the checked cases.
- Per-vector codebook-pair distance caches removed repeated VQ scoring during
  refinement and reverse seeding. Separate entries preserved the two floating-
  point subtraction orders, and entries distinguished complete errors from
  lower bounds returned by pruning. Cache allocation, initialization and lookup
  overhead outweighed the saved distance work on the checked music cases; an
  adaptive synthetic case also regressed.

Neither cache is present in the retained implementation. This result applies
to the bounded-distance encoder: avoiding repeated arithmetic is not sufficient
when many of those distances already exit early. Further performance changes
should be guided by measured hot paths and include complete encode timings,
not just counts of skipped computations.
