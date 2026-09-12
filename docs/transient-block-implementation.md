# Transient block switching: implementation prerequisites

Status: offline window analysis/synthesis validation implemented. No short/medium block encoder
is enabled by this change. Do not advertise pre-echo improvement from VQ SNR.

## Current blockers in this repository

- `encode_frame` forces window type 0 and Long. `mdct_channel` uses the same
  2N sine window and N-bin transform every hop.
- `quantize_main` selects Long codebooks and Long permutation/bit partitions.
- `quantize_gain_bark` produces one envelope and one channel gain. Short and
  medium frames require separate subblock envelopes and sub-gains.
- The current gain/VQ candidate scorer assumes one gain ratio for an entire
  channel and PPC subtraction in every frame. PPC must only be used in Long.
- Decoder support for nine window types already exists. Its synthesis changes
  overlap position and window size by type; changing only the encoded window
  bits will not produce a correct analysis transform.
- `bark_history_flags` in codec tests explicitly assumes Long frame layout and
  must be replaced with a window-aware parser when switching is implemented.

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

Next: derive bounded-lookahead analysis from this oracle, then implement
per-subblock Bark envelopes, gains and frame-type-dependent VQ/scoring before
enabling an experimental block-switching path. The production encoder still
emits long frames only; this stage does not establish a pre-echo improvement.
