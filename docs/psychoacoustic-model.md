# Experimental simultaneous masking model

Enable with `--psychoacoustic` or `Encoder::Config::psychoacoustic = true`.
The default is off; `--no-psychoacoustic` explicitly selects the existing
encoder objective. Native integrations using the default Config remain off.

This first model estimates relative frequency masking. It does not implement
an absolute hearing threshold, calibrated playback SPL, temporal masking,
transient detection itself. `--block-mode adaptive` provides a separate
experimental detector and block scheduler. The masking parameters are initial
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


## Rejected temporal threshold-growth experiment

A one-block limiter was evaluated with a cap of four times each ear/band's
previous unrestricted threshold. It updated from the original MDCT before
candidate trials, reset on silence or resolution changes, and recovered on
a steady spectrum. It was not retained; no temporal CLI option or Config
field is provided by this experiment.

The model's sequence invariance, onset response, recovery, silence and
stereo 80/96 codec checks passed. A synthetic noise onset on a quiet tonal
background gave only a negligible pre-attack error reduction while increasing
error during the attack. This did not justify adding another quality mode.
Private audio and its measurements are not included here.

The limitation is structural: final weights are normalized relative to the
mean threshold, so multiplying every threshold by a common factor cancels
out. A spectral threshold cap is therefore not an absolute noise constraint
and cannot directly constrain where reconstruction error occurs in time.

Next: evaluate an explicitly time-local reconstruction-error objective around
attacks, with gain and steady-region error guards. Keep the current relative
masking model and Long default until a temporal change has evidence of a
useful tradeoff; do not reintroduce this cap as a proven pre-echo improvement.


An independent [time-domain candidate-ranking experiment](time-domain-ranking.md)
is now available as `--temporal-search`. It does not reinstate the rejected
threshold cap; it scores reconstructed error after IMDCT/window synthesis.


## Experimental broadband voice protection

`--sibilant-protection` enables a separate relative error-weighting heuristic;
`--no-sibilant-protection` disables it. It is off by default, independently of
`--psychoacoustic`. The public encoder Config field is `sibilant_protection`.
The native component does not expose a new setting; the CLI does.

Per MDCT subblock, the detector combines the energy fraction and spectral
flatness between 2.5 kHz and min(9 kHz, 0.45 * sample rate). When broadband
activity rises, a tapered treble emphasis and a gentler 150–3500 Hz guard
multiply the existing error weights. The guard reduces competition against
the middle voice band. Identical M/S factors preserve channel symmetry.
The weighting participates in the existing Bark, PPC and main VQ searches;
it does not add bits or change the decoder/bitstream syntax.

This is not a speech or phoneme recognizer: cymbals and other broadband
sounds can also trigger it, and voiced speech without treble activity may
not trigger it. It is not a remedy for every vocal artifact. It can trade
error in other bands for lower treble error, including worse overall log
spectral distance. Listening validation is required before enabling it by
default. No source-specific measurements or recordings are distributed.

For an experimental, more expensive search at 80 or 96 kb/s:

```sh
vqf_encode -b 96 --vq-beam 32 --sibilant-protection input.wav output.vqf
```

Beam 32 is a separate search-quality/cost choice; the protection option also
works with the automatic beam. Existing defaults remain unchanged. The
`--test-mdct` checks include silence, tonal rejection, broadband activation,
weight bounds, gain/polarity/ear symmetry and composition with prior weights.


### Decoded LSP split refinement

The spectral LSP candidate performs one coordinate pass over its split-codebook
indices. Each trial is decoded with a private copy of predictor history and
scored using the existing mean-removed log-envelope distance. Only strict
improvements are accepted; only the winning indices update the persistent
history. Basic and angular frame candidates remain available to the final
reconstruction-error search.

This addresses a mismatch in the spectral search: split indices were
previously proposed only by angular distance, even though decoded envelope
distance ranked the completed candidates. The refinement costs extra CPU
and does not guarantee lower final PCM error: the LSP envelope is an
intermediate objective and later frames also depend on its history.
It follows default LSP search and respects `--no-lsp-search`. It is no
longer tied to `--sibilant-protection`.


### Frequency grid for envelope search

The spectral LSP distance uses 128 points spaced uniformly in
log(1 + frequency / 600 Hz), from zero through Nyquist, with a modest extra
weight between roughly 180 Hz and 4.3 kHz. This gives low/mid-frequency
envelope structure more evaluation points than a uniform-Hz grid. Both target
and reconstructed envelopes use the same grid; the mean log ratio is still
removed before scoring. Split refinement uses this objective too. This changes
candidate selection, not LPC order, transmitted indices, bit allocation or
decoder behavior. `--no-lsp-search` restores the previous non-spectral path.

The grid is a heuristic for envelope resolution, not a model trained on
Polish speech or a phoneme detector. Better envelope search may increase
encoding time by producing different frame candidates that require full VQ
search. `--sibilant-protection` still adds separate broadband treble weights
on VQ/Bark; those remain opt-in because they can trade noise-band error.

An additional experiment retained unrefined spectral frame candidates next
to refined candidates. It was not retained: it did not establish a useful
quality improvement. More candidates do not guarantee a better whole-track
result because the selected frame changes predictor history for later frames.
