# Experimental simultaneous masking model

Enable with `--psychoacoustic` or `Encoder::Config::psychoacoustic = true`.
The default is off; `--no-psychoacoustic` explicitly selects the existing
encoder objective. Native integrations using the default Config remain off.

This first model estimates relative frequency masking. It does not implement
an absolute hearing threshold, calibrated playback SPL, temporal masking,
transient detection, or automatic short blocks. Its parameters are initial
heuristics requiring listening-based tuning. Passing the regression tests
establishes numerical/bitstream correctness, not better perceived quality.

## Analysis

The model uses the original MDCT before any LPC, Bark, PPC or VQ operation.
All candidates in a frame share the resulting weights. Nothing is added to
the bitstream, the frame bit allocation stays fixed, and no new history or
delay is introduced.

1. Map bin centers to approximately one-Bark bands using
   `13 atan(0.00076 f) + 3.5 atan((f/7500)^2)`, with `f` in Hz.
2. Estimate mean power and spectral flatness (geometric/arithmetic mean) in
   each band. Map flatness to tonalness with `clamp(-log10(flatness)/3, 0, 1)`.
3. Place the local relative noise threshold 6 to 18 dB below mean power,
   depending on tonalness. Spread it at 12 dB/Bark toward higher frequencies
   and 27 dB/Bark toward lower frequencies. Take the strongest masker rather
   than summing all contributions.
4. Use a numerical floor of mean input power times `1e-12`. This floor is
   **not** a threshold of human hearing. Exactly silent frames use unit weights.
5. Let `T` be the resulting threshold and `R` its mean over bins. The error
   weight is `clamp((R/T)^0.25, 0.5, 2)`. This compressed, bounded inverse
   threshold deliberately limits how much the experimental model can change
   the objective. The maximum weight ratio between bins is four. The previous
   square-root weighting allowed a ratio of sixteen, with a more aggressive
   tradeoff against ordinary squared error. It is not a calibrated noise-to-mask ratio.

For stereo, reconstruct L/R spectra from M/S for analysis. Use the smaller
L/R threshold for both M and S. This conservative common weighting avoids
assuming that sound in one ear masks errors in the other; it also avoids
silently omitting the cross term of an unequal L/R quadratic metric. With a
completely silent ear, this can collapse to uniform weights. It is not a
binaural masking model.

## Encoder integration

The perceptual weights multiply the squared synthesis envelope already used
by main VQ, gain refinement, and whole-frame candidate scoring. They also
weight searched Bark envelope candidates. LSP candidate generation remains
angular/spectral; its final selection uses the new frame objective.

The fixed codebooks and bit allocation limit what can be changed. This is
noise shaping through candidate selection, not dynamic redistribution of
bits across bands. A lower ordinary SNR does not alone establish a regression
or an improvement in perceived quality; compare multiple independent metrics
and matched-level listening tests before changing the default.

For background on encoder-side perceptual decisions within a fixed decoder
syntax, see [RFC 6716, section 5.3](https://www.rfc-editor.org/rfc/rfc6716.html#section-5.3).
This implementation is an independent heuristic for TwinVQ, not the Opus
model, and its constants are not prescribed by that RFC.

## Reproduction

`--test-mdct` checks silence, bounded finite weights, gain/polarity invariance,
ear swapping, tonal/noise discrimination and spreading. The existing transform
and window-transition checks also run. `--test-codec-psychoacoustic` exercises
all 18 mode/channel combinations, irregular feed sizes, flush, finite PCM and
existing signal regression limits. `tools/test_vq_options.py` verifies the
opt-in switch, default equivalence and unchanged output byte counts.

For private 44.1 kHz stereo material, `tools/benchmark_music.py` accepts
`--masking off on --beams 16 --bitrates 80 96` along with the source,
`--encoder`, `--starts`, and an `--output-dir` outside the repository. Audio
and derived results must remain outside version control. The script reports
ordinary SNR/segmental SNR and log-spectral RMSE. The latter uses 1024-sample
Hann windows, both channels, and a magnitude floor 80 dB below the maximum
reference magnitude (at least `1e-15`). These are diagnostic metrics, not a
perceptual listening score.

Next steps are temporal stability/tonality analysis, calibration of the
relative masking curves, and integration with the forthcoming short-block
path. No pre-echo improvement is claimed for this stage.
