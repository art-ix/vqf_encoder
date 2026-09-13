# Further optimization candidates

These are engineering proposals for this encoder, not measured speed or
listening-quality guarantees. Changes to arithmetic/search must be validated
against complete output bytes or explicitly evaluated as quality experiments.

## 1. AVX2 kernel scheduling and specialization

[GCC's optimization manual](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)
documents loop unrolling, its code-size cost and the lack of a universal speed
benefit. The retained four-bin unroll targets the existing pruning interval
without changing accumulation order. A possible next experiment is specialized
kernels for frequently used vector lengths, selected from the generic path.
Measure instruction-cache cost as well as full encoding time.

[LLVM's llvm-mca documentation](https://llvm.org/docs/CommandGuide/llvm-mca.html)
provides throughput, resource-pressure and dependency analysis for assembly.
Use it to distinguish execution-port limits from a serial accumulation chain
before attempting to interleave independent candidate groups. Such interleaving
would have to preserve candidate order and cutoff semantics; it may do more
work because a later group cannot immediately use an earlier group's best score.
The CPU model is an estimate and does not replace runtime measurements.

## 2. Profile-guided release builds

[Microsoft's PGO workflow](https://learn.microsoft.com/en-us/cpp/build/profile-guided-optimizations?view=msvc-170)
uses /GL, /LTCG and /GENPROFILE (or /FASTGENPROFILE), representative training,
then /USEPROFILE. GCC provides -fprofile-generate and -fprofile-use, and -flto
for interprocedural optimization. Evaluate these as separate build experiments
with current floating-point constraints preserved. A training corpus should
cover mono/stereo, block modes and SIMD paths using redistributable synthetic
inputs; do not publish private audio or its profile data. PGO has not been
implemented by the current kernel change.

## 3. Speech-oriented LSP proposal weights

[RFC 6716 section 5.2.3.5](https://www.rfc-editor.org/rfc/rfc6716.html#section-5.2.3.5)
describes Laroia weighting of LSF errors and candidate-count tradeoffs in SILK.
For VQF, a possible experiment is an additional weighted LSP proposal, evaluated
alongside the current angular/spectral candidates with the existing bitstream.
This is an adaptation hypothesis, not a drop-in SILK quantizer or demonstrated
improvement for Polish speech. Any replacement of existing candidates needs
separate regression and listening evaluation.

Section 5.2.3.3 also describes speech noise shaping and formant-level matching.
Its filters cannot simply be transplanted into the VQF bitstream. A compatible
experiment would instead adjust encoder-side reconstruction-error ranking and
check formant levels, fricative energy and artifacts in time.

Recommended order: analyze/specialize the measured AVX2 hot loops, evaluate
PGO/LTO, then test LSP proposal weighting as a separate quality change. Avoid
combining these experiments, which would obscure the source of improvements.
