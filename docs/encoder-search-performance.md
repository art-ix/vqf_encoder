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

Forward and reverse beam seeding also use this packed layout and share the
ordered eight-candidate scoring helper with pair searches. Beam insertion
remains sequential and stable; its last entry supplies the group cutoff.
The runtime SIMD selection and scalar/SSE4.1 fallbacks are unchanged. The SIMD
test compares packed beam order and output boundaries against scalar selection
for widths 4/8/16/32, including partial candidate groups and ties. Encoder byte
comparisons and codec/parallel suites exercise the integrated path.

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

## Reusing reconstructed candidates

Within a Long-frame trial, scoring and gain fitting share the reconstructed
main codevectors. A local copy of the coefficient bytes identifies whether
reconstruction is still valid; changing gains alone does not invalidate it.
Restoring a different best candidate triggers reconstruction on the next use.
The first gain fit also reads the vector already reconstructed by initial
scoring, avoiding a separate allocation and dequantization. The cache cannot
cross trial boundaries, where mode or search inputs could differ.

PPC shape reconstruction runs once per trial before the channel loop, since
it decodes the joint shape for all channels. Per-channel period/gain synthesis
and subtraction retain their original order. Neither change alters scoring,
quantization, histories or transmitted coefficients.

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
compares complete VQF bytes. Its six cases cover mono with LSP/Bark searches
disabled, default stereo, Medium blocks with masking, adaptive blocks with
masking/PPC/temporal ranking, and voice protection with Long and adaptive
blocks; beam sizes 4, 8, 16 (auto) and 32 are exercised. Both executables must
support the voice-protection option.
It prints single-run timings as a local smoke benchmark, not a portable speed
guarantee. The codec suites cover all legal modes in mono/stereo, chunking,
flush, silence, duration and existing quality/pre-echo gates.

Byte equality is tested on the same build platform. It is not a promise of
identical output across compilers or floating-point architectures. Windows CI
also exercises the codec regressions.

## Earlier cache experiments

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

At that stage neither cache was retained. The combined LSP/PPC cache and
codebook-pair cache remain absent. This result applies to that version of
the bounded-distance encoder: avoiding repeated arithmetic is not sufficient
when many of those distances already exit early. Further performance changes
should be guided by measured hot paths and include complete encode timings,
not just counts of skipped computations.


## LSP result reuse after voice refinement

The opt-in voice mode added decoded split refinement and a denser low/midband
spectral grid. Repeated LSP quantization is consequently worth evaluating again.
The retained implementation now reuses LSP results within a single frame,
indexed by channel and the three search strategies. It stores all transmitted
LSP indices, decoded coefficients and the resulting predictor history.

Every candidate starts from the same frame target and prior history, so Bark,
PPC and gain variants can reuse these results exactly. Mixed mid/side strategies
reuse each channel independently. Temporal winner regeneration also uses the
same entries. The cache is local to `encode_frame`; no entry survives to another
frame, flush or encoder instance. No PPC or pair-distance cache was added.

This optimization preserves the search space, arithmetic of each LSP search,
quality settings and bitstream. Validation compares complete bytes with and
without voice protection, including adaptive blocks and temporal ranking.
Private reference material and its measurements are not distributed.


## Reusable frame scratch and cached tables

Per-trial heap traffic on the default path is now reused across frames:

- Bark band targets, LPC residuals, Levinson/LSP polynomials and PPC
  shape/gain/weight buffers sit on the stack. Envelope sizes fit the
  existing `kBarkEnvMax` / `kPpcShapeLenMax` limits.
- Trial residual, weights, envelopes, reconstructed vectors and the frozen
  VQ target/weight copies are Encoder members, sized once in the constructor.
- MDCT analysis writes the sine-windowed 2N buffer into `tmp_`. Long/Long
  LPC reuses that buffer instead of windowing again.
- PPC mu-law gains and the spectral LSP log-frequency grid are computed once
  per encoder, not per candidate.

Search decisions, beam widths and bitstream fields are unchanged. Complete
encoded bytes match the pre-change binary on the equivalence suite. Linux
single-run timings of two-second 44.1 kHz stereo / 96 kbps clips were about
4–6% lower on noise and attacks; short-file equivalence timings are too
noisy to treat as a portable speed guarantee.


## Reuse of LPC envelopes and normalized spectra

The per-frame LSP result also holds its reconstructed LPC envelope and the
input spectrum divided by that envelope, before PPC subtraction. These values
are initialized lazily after duplicate-trial rejection. They depend only on
the frame's original spectrum, block layout and channel/search LSP result;
Bark/PPC/gain alternatives can therefore copy them without repeating envelope
interpolation and per-bin division.

Each trial receives private copies: PPC subtraction and later residual/gain
updates cannot mutate the cached data. The envelope floor and division order
are unchanged. All storage is local to the frame, including temporal winner
regeneration, so block transitions and later predictor histories cannot reuse
stale values. This adds bounded per-frame vectors for visited LSP strategies
and removes the separate per-trial decoded-LSP vector.

Validation uses complete-byte comparisons with the previous encoder for six
synthetic configurations, including protected adaptive/temporal search. The
optimization does not change quality weights, search width or transmitted bits.


## AVX2 pruning-interval unrolling

The packed-candidate distance kernel now explicitly processes four frequency
bins before testing its existing cutoff. Each SIMD lane still represents one
candidate, and all additions and multiplications retain the original order.
There are no independent partial sums or fused multiply-add substitutions.
The remaining one to three bins use the same single-bin helper. Padded lanes
remain excluded from pruning; candidate selection and tie handling are unchanged.
The same helper serves both codebook scans and beam initialization.

Existing SIMD checks compare partial lengths, signed candidates, ties,
unaligned input, padded groups and beam results against the scalar path.
Full-stream equivalence also checks optional voice protection and temporal
ranking. Gains must be measured on the actual compiler/CPU: unrolling is not
universally beneficial and increases code size.

A separate attempt to cache joint PPC results across Bark alternatives was
retested after the voice changes. It preserved output bytes but did not
establish a timing benefit and was removed. Profiling then directed attention
to the packed AVX2 searches. Private recordings and profiling measurements
are not included in this repository.

See [optimization research](optimization-research.md) for primary-source
references and the remaining proposed experiments.

## Cached decoded gains and ordered sub-gain search

Baseline: `e770cadffc1cacc456df3b7cb7989da1e3f11d68`.
Short/Medium gain fitting previously recalculated the mu-law expansion and
scanned all 32 sub-gains for each of 256 global gains. A shared, immutable
32 KiB table now stores the exact float product for each transmitted pair.
The encoder's gain decoder reuses that table; Long gain decoding reuses the
existing 256-entry Long table. No decoder or bitstream format changes.

For a fixed global gain, decoded sub-gains are ordered. Search starts at the
previous global gain's selected sub-gain and walks neighboring values in both
directions using exact squared-error comparisons. The hint usually moves
little, avoiding a repeated binary or full search. Equal-error plateaus retain
the lowest index, including
plateaus from floating-point rounding on very large targets. All 256 global
gains remain searched in their original order, with unchanged error summation
and tie rules. The gain search has no effect on VQ beam width or quality effort.

`--test-gains`, included in `make test`, checks 406528 hinted queries against an
independent exhaustive scan, requiring identical indices and squared errors.
Cases cover exact gains, midpoint ties and adjacent representable doubles,
random targets, repeated gain values, extremes, infinities and NaN. Full-stream
reference comparisons are necessary too: they check cached table rounding and
its integration with both gain levels, Bark histories and VQ searches.

Validation uses the same compiler and floating-point flags for the baseline
and candidate:

```sh
make test
python3 tools/test_encoder_equivalence.py bin/vqf_encode /path/to/baseline
```

All six synthetic full-stream equivalence cases passed, including temporal
search, PPC, masking and voice protection. Speed depends on how often gain
fitting runs: improvements with forced Short/Medium blocks do not imply the
same overall speedup with adaptive block selection. Measure representative
inputs with alternating baseline/candidate runs and report repeated timings.
