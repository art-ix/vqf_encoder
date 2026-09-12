# Transient block switching: implementation prerequisites

Status: implementation prerequisites. No short/medium block encoder
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

