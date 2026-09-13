# Tonal quantization-noise protection

The unweighted reconstruction objective can spend too little effort on quiet
spectral gaps between strong partials. On plucked, decaying sounds, added
energy in those gaps can become audible as a separate hiss. This is an
encoder-side noise-shaping hypothesis; transient smearing remains a separate
possible artifact, and forcing Short blocks can worsen tonal reconstruction.

`Encoder::Config::tonal_protection` is enabled by default. The CLI accepts
`--tonal-protection` and `--no-tonal-protection`; the latter restores the
previous objective. It is independent of the opt-in `--psychoacoustic` and
`--sibilant-protection` options. Native clients using the default Config also
receive the protection.

## Analysis and integration

Each original, unquantized MDCT subblock is analyzed before LPC/Bark/PPC/VQ
search. Sum squared M/S coefficients (mono uses one channel), then estimate
spectral flatness within approximately one-Bark bands. A relative numerical
floor is the overall mean power times `1e-12`; it is not a hearing threshold.

- Treble activity ramps from zero to one as the energy fraction above 2 kHz
  rises from `1e-5` to `1e-4`. This avoids reacting to numerical spectral tails
  of signals with essentially no treble. Silence returns unchanged weights.
- Band tonality ramps from zero at flatness 0.25 to one at 0.05. These are
  heuristic, bounded settings, not calibrated psychoacoustic thresholds.
- Smooth power over five MDCT bins to avoid overreacting to a single
  phase-dependent coefficient null. The valley term is the square root of
  band mean power divided by smoothed power, clamped to [1, 3].
- Frequency emphasis rises linearly from zero at 1 kHz to one at 2.5 kHz.
  Multiply existing error weights by
  `1 + activity * tonality * frequency_emphasis * valley_term`, in [1, 4].

Both M and S use the same factors, preserving ear symmetry. The existing
weighted Bark, gain, VQ and final candidate-ranking paths consume the result.
No input filtering, output denoising, low-pass cutoff, added bits, changed
block schedule or decoder modification is introduced. No cross-frame state is
added; silence and resolution changes cannot retain a stale masking history.

Weights are relative priorities under a fixed bit budget. Ordinary waveform
SNR can fall while error in exposed spectral gaps improves. The detector is
not a guitar recognizer and may also activate on other harmonic instruments
or voiced speech. This change does not claim to eliminate all hiss or pre-echo.
Matched-level listening remains necessary to judge the tradeoff.

## Validation

`--test-mdct` includes checks for silence, flat spectra, activation on harmonic
spectra, bounded finite weights, gain and polarity invariance, ear swapping,
composition with existing weights, and reset after activity.
`tools/test_tonal_protection.py` checks default activation, explicit disable,
bit budget and scalar/SIMD/thread agreement at 80 and 96 kb/s.
The complete codec suite covers all supported modes, block lengths, adaptive
transitions, temporal/PPC search, chunking and flush.

For reference comparisons, use baseline commit
`8c6192391dd9fcca9122f95ba718be3bb450ecb9` and the same build options. Disabling
tonal protection must produce the baseline bytes; enabling it intentionally
changes quantization decisions. The existing
`tools/benchmark_gain.py BASELINE CANDIDATE OUTPUT_DIR` provides deterministic
synthetic signals and independent FFmpeg decoding for diagnostic SNR checks.

For recording-specific hiss analysis, encode from the beginning before
extracting evaluation regions: seeking the input changes transform alignment
and predictor history. Freeze spectral-peak/gap masks from the reference only,
then measure both added energy in gaps and errors/levels at the peaks. Check
other passages and noise-like sources as controls. One diagnostic uses
2048-sample Hann windows at a 512-sample hop, separately in L/R, from 2 to
10 kHz. Mark reference bins at least 15 dB below their nine-bin local peak,
excluding local peaks below `1e-6` of the reference maximum power. Sum
`max(decoded_power - reference_power, 0)` only in that fixed mask. This
measures excess gap energy, not perceived loudness or isolated noise power.
Keep private recordings,
excerpts and their measurements outside version control.

The deterministic synthetic benchmark at default settings gave these changes
in ordinary waveform SNR (dB): tones 0.000, harmonics -0.313, attacks -0.064,
noise 0.000, and fade/identical/antiphase/left-only approximately 0.000. These
measurements expose the fixed-budget tradeoff; they are not listening scores.
The treble-activity guard removed a -0.580 dB loss on the simple-tone fixture
seen in the ungated prototype. That prototype is not retained.
