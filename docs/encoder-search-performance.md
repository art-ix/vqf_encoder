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
