# Encoder quality audit — 2026-09-12

Reviewed source: main commit `18aaba46a07037bb15ccc02e3bfe01070d770390`.
Working branch: `encoder-quality`, created from that main commit.
Remote commit/ref writes were verified with empty commit `b98a9c660b69e51446e568079d825440c2845fac`.
This audit changes documentation only.

## Baseline verified in this session

Built with the repository Makefile and g++ on Linux. Commands:

```sh
make -j4 test
node tools/test_wav.mjs bin/vqf_encode
```

Both succeeded. MDCT direct-oracle maximum error: 6.35628e-8;
overlap-add: 4.76837e-7; IMDCT: 4.65661e-10.
All 18 mode/channel combinations passed codec regression checks.
At 44.1 kHz stereo / 96 kbps the codec-test chirp achieved 13.659 dB SNR,
-0.731803 dB gain and 2.29327e-5 silent-input peak.
The separate 0.5-second roundtrip test achieved 20.3841 dB SNR,
0-sample lag and 0.998751 correlation. These are different synthetic signals;
their scores are not comparable as an improvement.
All six WAV-format cases and the malformed-input checks passed.

The previous fixes described in encoder-quality.md are already present:
corrected LPC-to-LSP analysis, synthesis-envelope-weighted VQ, beam search,
gain fitting, fast MDCT and delay corrections. They should not be proposed as
new work. Earlier music-recording measurements were not reproduced here:
the original PCM/FLAC is not in the repository. No listening test or independent
FFmpeg decoding was performed in this audit.

## Findings and proposed work

### 1. CLI resampling is an avoidable quality loss

Location: tools/vqf_encode.cpp, resample_linear (lines 103–124), call near 350.
It interpolates two adjacent samples without a proper anti-alias low-pass.
pick_encoder_mode maps rates >= 32000 to 44100, so this affects common
48/96 kHz inputs even at maximum bitrate. At 88200 -> 44100 the algorithm
takes every second sample exactly: an input 30 kHz tone aliases to 14.1 kHz.
This is a consequence of the implementation, not a measurement made here.
Native-rate 44100 input bypasses this path.

Replace it with a band-limited polyphase/windowed-sinc resampler, with defined
passband, stopband, delay and boundary handling. Compare against an independent
resampler using passband sweeps, above-Nyquist tones and impulses.
The foobar2000 component uses the host resampler; this finding applies to CLI.

### 2. Only long blocks are encoded

Location: twinvq/src/twinvq_encoder.cpp, encode_frame near 958,
quantize_gain_bark near 757 and quantize_main near 828.
Every frame sets window_type=0 and FrameType::Long. At 44.1 kHz the hop is
2048 samples (46.44 ms), and the analysis window spans 4096 (92.88 ms).
This creates a structural risk of pre-echo and smeared attacks on percussion,
plucked strings and abrupt starts. Actual audibility remains to be measured.

Implement transient detection and lookahead, legal window transitions,
short/medium MDCT analysis, per-subblock Bark/gain quantization and matching
frame-type VQ. Decoder support exists, but setting the window bits alone is
insufficient. Verify every supported transition with overlap-add/impulse tests
and independent decoding before judging musical quality.
For stereo, transient decisions must consider both channels, including side.

### 3. PPC is a stub, not a pitch search

Location: quantize_ppc near 820; decode_ppc near 581;
init_bitstream_params near 351; encode_frame near 1018.
Period and gain indices are fixed to zero, and PPC shape indices are zeroed.
Long-frame PPC bits are still reserved and written, so the encoder does not
exploit these parameters for harmonic structure.

The comment "send silence" is not literally accurate: decode_ppc reconstructs
gain using a half-step even for index zero, and zero shape indices select
codebook entries rather than a zero vector. The encoder does reconstruct and
subtract that PPC contribution before main VQ, so this is not an unaccounted
decoder mismatch. Its magnitude/impact needs targeted measurement.

Search period candidates, quantize shape with the actual PPC codebooks, fit
quantized gain, then evaluate PPC plus main-VQ reconstruction together.
Always retain the current solution as a candidate; voiced material may benefit,
but noisy signals need not. Keep codebooks and bit allocation compatible.

### 4. LSP search ignores prediction during its early decisions

Location: quantize_lsp near 685 and decode_lsp near 510.
The first-stage codeword is chosen alone, then split residuals are quantized.
Only afterward are history choices tested. Yet decode_lsp mixes reconstructed
LSPs with history and rearranges/sorts them, so the early Euclidean target is
not the final decoded target.

Try a beam of first-stage candidates for each history choice, refine splits,
and score the decoder-reconstructed envelope. Begin with spectral-envelope
distortion, then evaluate the full residual reconstruction. Snapshot history
for candidates and commit only the winner. Avoid claiming that smaller raw
LSP distance necessarily means better audible quality.

### 5. Bark history and joint envelope optimization are unused

Location: quantize_gain_bark near 757 and dec_bark_env near 490.
bark_use_hist is always zero. The codebook fit gives each band equal error
weight, without band width, LPC synthesis amplification or masking thresholds.
The target also assumes a fixed codebook RMS before main VQ is known.

Evaluate history on/off with independently optimized codebook candidates.
Try decoder-domain weighted band error, then a bounded Bark/gain/main-VQ
iteration. Histories must be restored between trials; dec_bark_env mutates
them even while evaluating a candidate.

### 6. Main VQ optimizes waveform error, not masking

Location: quantize_main near 828 and encode_frame near 1038.
Weights are (LPC envelope * Bark gain)^2. This correctly accounts for
synthesis amplification, but is not a psychoacoustic masking model.
The beam width is fixed at four, followed by two coordinate-refinement passes.

First measure quality/speed for larger beams and an exhaustive pair-search
oracle on selected frames. Expose an effort setting without changing bitrate.
Then experiment with frequency-dependent masking thresholds, tonality and
temporal protection. Keep thresholds floored and evaluate audible regressions;
higher SNR is not sufficient evidence for a psychoacoustic improvement.

Gain is fitted once and main vectors are searched again. The original
gain/vector pair is not retained as a fallback, and approximate VQ can select
a worse final candidate. Compare complete reconstructed error before accepting
each iteration; also try adjacent quantized gains.

## Suggested implementation order

1. Establish a reproducible PCM corpus and metrics; replace CLI resampling.
2. Add bounded LSP/Bark/gain search improvements and tunable VQ effort,
   retaining the baseline candidate. Measure quality against encode time.
3. Implement transient block switching: highest structural priority for attacks,
   but a larger compatibility-sensitive change.
4. Implement PPC search and evaluate harmonic material.
5. Tune perceptual weighting on a separate validation set and blind listening.

## Acceptance criteria

Use uncompressed source clips: percussion/castanets, speech, sustained tones,
piano, dense music, cymbals/noise, silence/fades and stereo edge cases
(L=R, L=-R, one silent channel). Existing VQF recordings can test decoding,
but decoding/re-encoding them is not a clean original-source quality benchmark.

Track aligned waveform and segmental SNR, band/log-spectral error,
pre-attack noise energy, gain/peak/clipping, stereo leakage and encode time.
Preserve duration and report raw delay/polarity before any metric alignment;
the current comparison script can otherwise conceal timing/polarity regressions.
Measure attack regions explicitly rather than relying on whole-track SNR.
Use FFmpeg as an independent compatibility check, all 18 modes for regression,
and blind A/B or ABX listening for audibility. ABX establishes detectability;
a preference test is needed to claim that listeners prefer the new version.
Do not modify decoder tables or invent higher-bitrate modes as a quality fix.


## Implementation update: band-limited CLI resampling

The first implementation replaces linear interpolation with a centered
Blackman-windowed sinc filter (96/cutoff samples of support on each side).
The cutoff is 95% of the lower Nyquist frequency. Phase kernels are normalized
for DC, precomputed with up to 1024 phases, and linearly interpolated where
needed. Integer phase accumulation avoids duration-dependent timing drift.
Endpoints use constant extension; the filter introduces no sample-zero offset.
Extremely large downsampling ratios requiring support above 4096 input samples
are rejected. There is no new library dependency or bitstream change.

Linux validation after the change:

- `make -j4 test`: passed, including the new `--test-resample` suite and all
  existing codec/MDCT/LPC tests.
- `node tools/test_wav.mjs bin/vqf_encode`: passed.
- Seven rate pairs cover 48/88.2/96 -> 44.1 kHz, 32 -> 44.1 kHz,
  24 -> 22.05 kHz, 12 -> 11.025 kHz and 6 -> 8 kHz.
- Analytic tones at 5%, 50% and 90% of the lower Nyquist frequency:
  worst relative RMS waveform error below 0.000110 (0.011%).
- Tested downsampling stopband tones: worst relative RMS below 3.02e-5,
  approximately 90.4 dB rejection; the regression gate is 80 dB.
  Includes the explicit 30 kHz tone at 88.2 -> 44.1 kHz.
- Constant signals, single-sample and empty inputs, native-rate identity,
  channel isolation, output length and impulse timing passed.
- One-second stereo CLI inputs at 48/88.2/96 kHz encoded and independently
  decoded by FFmpeg to 45056 finite PCM frames each (44100 source-equivalent
  samples plus codec tail padding). Peaks: 0.285392 / 0.285356 / 0.285268.
  FFmpeg exited successfully but still logged the previously documented
  end-of-input VQF demux error; this change does not fix container tail handling.

The new regression command is also added to the Windows CI workflow; Windows
results are not claimed by these local Linux measurements. These tests establish
resampling behavior, not perceived quality improvements on music. Native-rate
44.1 kHz inputs bypass this resampler. The native foobar2000 component continues
to use its host resampler. LSP/Bark/VQ, transient switching and PPC work remain.


## Implementation update: bounded gain/VQ refinement

Baseline: b736ac05e51b8efb0237aa2d94c0ebf2f4dc7f93.
The encoder now retains both the initial VQ solution and the previous encoder's
fitted-gain/requantized solution. Candidate errors are compared in a fixed,
synthesis-weighted MDCT objective against the same target. Up to two additional
iterations search all 256 decoded gain values per channel for the current
vectors and rerun VQ. The fixed-vector gain result is retained too. Trials stop
when gains do not change or the objective does not improve. The best complete
pair of gains and main-codebook indices is restored before writing the frame.
LSP/Bark/PPC histories are unaffected by these trials.

This prevents a candidate with higher modeled frame error from replacing a
better candidate, including the legacy result. It is not a guarantee of lower
PCM error in every time interval or improved perceptual quality: overlap-add,
finite precision and auditory masking are distinct from this objective.

Validation on Linux:

- MDCT/LPC tests, all 18 codec modes (including chunked feed and silence),
  resampler tests and WAV-format tests passed.
- The 0.5-second roundtrip SNR rose from 20.3841 to 21.2856 dB, with zero lag.
- Independent FFmpeg decoding of eight two-second synthetic stereo clips at
  44.1 kHz / 96 kbps yielded the following aligned, fixed-polarity PCM scores.
  No delay search or gain normalization was applied; 4096 samples at each end
  were excluded. These are synthetic diagnostics, not music listening results.

| Signal | Before SNR (dB) | After SNR (dB) | Delta (dB) | Encode time ratio |
| --- | ---: | ---: | ---: | ---: |
| tones | 14.351 | 14.470 | +0.118 | 1.02x |
| harmonics | 27.365 | 27.365 | +0.000 | 1.09x |
| attacks | 9.937 | 10.494 | +0.557 | 1.89x |
| noise | 4.333 | 4.601 | +0.268 | 1.94x |
| fade | 17.859 | 17.859 | +0.000 | 1.06x |
| identical | 17.652 | 17.729 | +0.077 | 1.11x |
| antiphase | 17.652 | 17.729 | +0.077 | 1.04x |
| left-only | 17.536 | 17.536 | +0.000 | 1.00x |

All compared files are 25686 bytes and decode to 90112 frames including tail
padding. FFmpeg still reports the known end-of-file demux error for both
versions; decoded samples are finite. Timing is a single run per clip and is
indicative, not a controlled performance benchmark. Difficult attack/noise
inputs approach twice the previous encoding time. No listening-quality claim
is made from these SNR results.

Reproduce with separately built baseline and candidate executables:

```sh
python3 tools/benchmark_gain.py /path/to/baseline/vqf_encode bin/vqf_encode
```

The script requires Python and FFmpeg only for measurement; neither is linked
into the encoder. It checks finite decoded output, unchanged encoded size and
decoded duration, and writes its generated clips/results under obj by default.
LSP/Bark search, transient block switching and PPC remain subsequent work.


## Implementation update: optional frame-scored LSP search

Baseline: 49f95857faae111c13e6ab62ad43db6f91374353.
This implementation is **opt-in**, via CLI `--lsp-search` or C++
`Encoder::Config::lsp_search`. Default configuration retains the baseline
algorithm, including gain/VQ refinement. The foobar2000 component does not
expose the option yet.

For each history index, the search keeps eight first-stage LSP candidates,
fits split residuals with the decoder's history prediction weights, then scores
actual decoded LSPs including rearrangement/sorting. The original LSP candidate
is included. Trial decoding uses separate history snapshots.

Choosing solely by LSP coefficient error caused regressions: the synthetic
fade score dropped 0.655 dB, and some mono codec tests worsened. The implemented
option therefore compares two complete frame quantizations: ordinary LSP and
searched LSP, with their respective LPC envelope, Bark, gain and main VQ.
The lower synthesis-weighted MDCT error wins. Gain/VQ refinement is identical
in both trials. Frame parameters AND the winning LSP/Bark histories are restored
before writing exactly one frame. Input overlap advances once, and the growing
output file and pending PCM are not copied between trials.

This is a local decision from a shared starting history, not a guarantee that
an entire track matches or beats a separately running baseline. Chosen history
changes future frames, and the modeled frame objective differs from cropped
PCM SNR after overlap-add. Remaining regressions and extra encode cost are why
the feature is not the default.

Synthetic 44.1 kHz stereo / 96 kbps results, independently decoded by FFmpeg:

| Signal | Baseline SNR (dB) | Search SNR (dB) | Delta (dB) | Time ratio |
| --- | ---: | ---: | ---: | ---: |
| tones | 14.470 | 15.022 | +0.552 | 1.91x |
| harmonics | 27.365 | 27.495 | +0.129 | 2.02x |
| attacks | 10.494 | 10.699 | +0.205 | 1.94x |
| noise | 4.601 | 4.609 | +0.008 | 1.89x |
| fade | 17.859 | 17.600 | -0.259 | 1.90x |
| identical | 17.729 | 21.620 | +3.891 | 1.93x |
| antiphase | 17.729 | 21.620 | +3.891 | 1.93x |
| left-only | 17.536 | 19.611 | +2.075 | 1.87x |

The existing codec chirp at 44.1 kHz stereo / 96 kbps rose from 13.659 to
14.8223 dB. However, the 11.025 kHz stereo / 20 kbps chirp fell from 22.947 to
22.153 dB. These results do not establish perceived music quality. Encoding
costs approximately 1.9–2.0 times the baseline in these single-run timings.
Encoded sizes and decoded durations remain identical in all eight clips;
FFmpeg still emits the known end-of-file demux warning in both versions.

Validation:

- Linux build and explicit MDCT/LPC, resampler, default codec, enabled codec,
  roundtrip and WAV input-format tests passed.
- The new `--test-codec-lsp` command runs all 18 mono/stereo combinations with
  the option enabled, including history-sensitive irregular feed chunking,
  repeated flush, duration, gain, finite output and silence checks.
- All eight benchmark clips were re-encoded after introducing the option:
  default output matches the baseline byte-for-byte, and enabled output matches
  the measured search candidate byte-for-byte.

Reproduce comparison with:

```sh
python3 tools/benchmark_gain.py /path/to/baseline/vqf_encode bin/vqf_encode obj/lsp-comparison --lsp-search
```

Further work should score temporal reconstruction/history consequences before
promoting the option to default. Bark history search, block switching and PPC
are still pending; this change evaluates existing Bark quantization as part of
LSP selection but does not add new Bark candidates.
