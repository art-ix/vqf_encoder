# Relative mid/side noise protection

A low-energy side component can accumulate substantial relative quantization
error while contributing little to the ordinary L/R error sum. On a largely
centered signal, poorly reconstructed side information can sound like a
separate noisy stereo component. This hypothesis is distinct from pre-echo
and inter-partial noise; it does not explain every audible artifact.

`Encoder::Config::stereo_noise_protection` defaults to true, enabled at the
user's request after the experimental comparison. The CLI provides
`--stereo-noise-protection` and `--no-stereo-noise-protection`. Disabling it
restores the previous search objective; tonal and broadband protection retain
their independent settings. Mono is unchanged.

## Objective

Analyze original planar M/S MDCT coefficients independently for every
subblock, using the same approximate one-Bark geometry as the existing
perceptual models. Sum energy for M and S in each band. Let `R` be the larger
of those two band energies, `E` the selected component's energy, and `F` the
whole subblock's M+S energy times `1e-10`. The relative multiplier is
`clamp(sqrt(R / max(E, F)), 1, 4)`. Exactly silent subblocks return unchanged
weights. Empty bands and balanced M/S bands have multiplier one.

Taper the extra weight from zero below 1 kHz to full strength above 2.5 kHz,
then multiply the existing perceptual weights. The stronger component is
never downweighted. Weak M receives the same treatment as weak S, so there is
no arbitrary preference for in-phase over antiphase content. Ear swaps,
polarity and overall level preserve the weights. Calculations have no
cross-frame state, and band factors are computed once per subblock.

These unequal M/S weights deliberately penalize relative spatial error.
They are not a claim about interaural masking or an approximation to separate
L/R weights: in L/R coordinates this objective includes a cross term. Existing
Bark, PPC, gain, VQ and frame-candidate scoring consume the resulting weights.
No stereo narrowing, input filtering, output denoising, extra bits, changed
block schedule, decoder change or additional delay is introduced.

The fixed bit budget creates a tradeoff. Error in the stronger component can
increase while the weaker one improves, so evaluate M and S separately as
well as the full stereo signal. A reduced side error does not imply a reduced
whole-signal spectral-gap metric or establish a listening preference.

## Checks

`--test-mdct` tests silence, mono, balanced M/S, frequency taper, bounded
weights, scaling, polarity, ear swapping, weak-M symmetry, composition and
reset on silence. `tools/test_stereo_noise_protection.py` checks the default,
explicit disable, bit budget, SIMD and threading at stereo 80/96 kb/s. It runs
in `make test` and the Windows workflow. Existing codec tests exercise block
modes, adaptive transitions, chunking, flush and other search options.

Reference baseline: `007d6ffd0fdb548ba344762c098dcfe2f49ea21b`. Compare disabled
output with that baseline byte for byte using equal compiler/float options.
For recording-specific diagnostics, encode from the beginning before cropping
the decoded PCM; preserve transform alignment and history. Inspect reference,
reconstruction and error in M=(L+R)/2 and S=(L-R)/2, report both energy excess
and reconstruction error, and verify other passages. Keep recordings,
excerpts and recording-derived measurements outside Git.
