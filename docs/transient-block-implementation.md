# Transient block switching: implementation prerequisites

Status: fixed short/medium encoding and adaptive Long/Short scheduling are
implemented. Adaptive is the default: Long frames until a transient, then
Short. `--block-mode long` restores the previous path.

## Fixed-block implementation

Use `--block-mode short` or `--block-mode medium`; the default is `adaptive`.
The C++ API exposes `Encoder::Config::block_mode` with `Encoder::BlockMode`.
Fixed short blocks are evaluation modes, not a recommendation to encode an
entire music track with short windows.

- Production analysis transposes decoder overlap/copy operations using two
  adjacent PCM hops and the next window geometry. It is compared against the
  independent whole-schedule oracle below. No additional PCM delay is added.
- The fixed schedule starts with window 0, repeats 2 (Short) or 8 (Medium),
  and flushes with 3 or 5 respectively. The final partial input hop is padded
  before the exit frame. First-frame priming and output length are preserved.
- LPC is transmitted once per channel; its envelope is reconstructed at the
  subblock resolution. Bark indices, history and gains are selected per subblock.
  History follows decoder order and is committed only for the winning trial.
- Global and sub-gain combinations are searched jointly in decoder units.
  Two main-VQ passes retain the best reconstruction, including a final gain
  fit. VQ codebooks, permutations and bit partitions follow the frame type.
  PPC is used only for Long frames. Candidate snapshots include sub-gains.
- Optional masking weights are computed independently per subblock, with
  the matching frequency resolution and both stereo channels.
- Each frame must write exactly its specified bit count before byte packing.
  The default Long path remains the control for bit-identical comparisons.

`--test-codec-short` and `--test-codec-medium` cover all 18 mode/channel
combinations, fixed window schedules, history flags, chunked input, flush,
finite output, silence and reconstruction gain. Both use masking enabled.
Targeted external decoding also covers 80 kbps Short and 96 kbps Medium.
These checks establish implementation correctness, not subjective improvement.

## Implementation order and gates

1. Derive analysis windows and legal transitions from the decoder's actual
   `imdct_and_window` and `imdct_output` geometry. Validate analysis/synthesis
   without quantization: impulses at every transition offset, tones, noise;
   boundary and flush cases. Long -> short -> long and medium transitions must
   reconstruct at correct amplitude and alignment, including file boundaries.
2. Generalize frame encoding/scoring to frame type, subblock count and window
   type. Implement subblock gain quantization and frame-type Bark codebooks,
   history, permutation and main-VQ bit allocation. Preserve Long-only output
   as a control. Confirm exact frame bit counts independently of padded bytes.
3. Add bounded lookahead and transient detection considering both L/R channels
   (and side-only attacks). Preserve whole-buffer/chunked equivalence and report
   any additional encoder delay explicitly. Schedule legal transition windows.
4. Enable switching behind a comparison flag first. Decode with the local
   decoder and FFmpeg. Use impulse/pre-attack energy tests as well as music
   listening; aggregate SNR alone is insufficient for temporal artifacts.


## Stage 1: window geometry and offline analysis oracle

`twinvq/src/twinvq_window.hpp` now owns the window-type mapping and the overlap/copy
layout used by the decoder. Extracting it preserves the previous synthesis
operation order within each window. The decoder computes each subblock IMDCT
before applying the layout.

`--test-mdct` also runs `window_transition_self_test`. Its offline analysis
transposes the decoder's overlap rotations and copies, working backwards over
a complete schedule. A forward MDCT of the middle-half buffer then inverts the
half IMDCT. This is a reference for deriving streaming analysis; it is not yet
an encoder feature or a transient detector. For ordinary long windows, its
coefficients are also compared with the existing full sine-window MDCT.

The suite covers all nine window IDs and every supported mode, with mono and
stereo transform normalization. Tested schedules include:

- `0, 0, 2, 2, 3, 0, 0`: long -> short -> long;
- `0, 0, 8, 8, 5, 0, 0`: long -> medium -> long;
- `0, 1, 2, 4, 7, 5, 6, 0`: short -> medium and alternate window IDs;
- `0, 0, 8, 2, 3, 0, 0`: medium -> short;
- `0, 0, 2, 3, 0, 0`: a single short frame.

Noise, tones, DC, boundary impulses and zero-padded ends pass the transform
roundtrip. An additional window-only impulse sweep covers every offset in
active frames for each distinct mode geometry. Changing the synthesis exit
window without changing analysis must fail the identity check. These tests
validate the listed schedules, not a complete transition-state machine or the
encoder's feed/flush behavior with future lookahead.

Validation on Linux: maximum transform/overlap error `6.56e-7` (limit `5e-6`),
18 codec mode/channel regressions passed. Synthetic bitstreams exercising all
nine window IDs decode to byte-identical PCM before and after extraction in
all 18 mode/channel combinations. No music material or music-derived results
are included here.

Next: tune the adaptive detector and block decisions using temporal error
measurements and listening; do not infer overall quality from one metric.


## Adaptive Long/Short scheduling

`--block-mode adaptive` (`Encoder::BlockMode::Adaptive`) uses original L/R
PCM in Short-sized slices. Per-channel mean-square energy and first-difference
energy are compared with causal fast-attack envelopes with a 15 ms release.
The reference rises immediately to the current energy and decays exponentially;
a smoothed average can lag an existing attack and trigger again during its
rise. This avoids extending Short runs solely because the reference has not
caught up. Initial trigger ratios are
8 and 12 respectively, with a numerical floor of `1e-10`. They are heuristics,
not calibrated hearing thresholds. No channel averaging hides side attacks.
Onset from digital silence (including the leading delay hop) does not count
as an attack: there is no audible pre-echo into preceding silence.

A one-hop queue gives the scheduler the upcoming attack regions. Each frame
honors the window already used by the previous frame's analysis; only its
successor is selected. The legal subset is 0 -> 2, 2 -> 2/3, 3 -> 0/2, and
0 -> 0. The detector distinguishes early and late attack slices. A Short
frame covers the late region of the buffered PCM hop and the early region
of the lookahead hop. The boundary follows decoder geometry:
`(N - short_block_size / 2) / 2`. A one-slice guard on both sides retains
two Short frames for attacks near that boundary; otherwise one is sufficient
to cover the onset region. This avoids sacrificing frequency resolution in
an adjacent frame that does not contain the onset. EOF retains the existing
closure policy rather than scheduling a new Short flush frame.

When both the current and next windows are type 0, analysis uses the same
sine-window MDCT as `--block-mode long`. Transition and Short layouts use
pair analysis.

The extra buffering is one hop, reported by `lookahead_samples()` and CLI.
It does not add output samples or another priming frame. Memory is bounded,
and irregular feed calls make the same decisions as whole-buffer input.
`--block-mode long` bypasses both the queue and detector.

The window oracle includes repeated Short exits/reentries. The targeted
adaptive codec suite covers stereo 80/96 kbps, early/central/late side-only
attacks, transmitted window placement, release,
window legality, chunking, decoded length, short/empty input and pre-attack
energy with an attack-gain guard. External FFmpeg decoding is also checked
on synthetic and private local inputs. Private audio and measurements stay
outside Git. Lower pre-echo can still trade off against other reconstruction
errors; no listening improvement is claimed.


The central-attack test retains its pre-echo improvement gate. A late tonal
attack instead checks leakage relative to attack energy and reconstructed
gain: Long can have less leakage for that fixture even before positional
selection. Local comparison with the two-Short-frame baseline is needed to
distinguish a scheduling regression from this existing tradeoff.
