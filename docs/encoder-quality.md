# Encoder quality and speed checks — 2026-09-12

The encoder previously fitted LPC polynomials in the wrong Chebyshev coefficient
order, then minimized vector-quantization error without accounting for the LPC
and Bark synthesis gains. This made spectral errors grow in emphasized bands.
It also attenuated large residuals without transmitting a matching gain change.

Changes:

- Correct LPC-to-LSP coefficient order, constant-term weight and alternating
  root search; use double precision for the root polynomials.
- Score main-codebook pairs with the squared synthesis-envelope weight. Keep
  four first-stage candidates and refine both codebook choices twice.
- Fit each channel's transmitted gain to its reconstructed vectors, then
  requantize. Account for the decoder's half-step gain reconstruction.
- Remove the untransmitted residual attenuation.
- Replace the quadratic forward MDCT with a transform of order N log N,
  with cached rotations and analysis windows. Verify against the direct formula.
- Process complete input hops directly instead of copying the remaining track
  after every hop.
- Correct mono amplitude and account for the MDCT's own priming hop, eliminating
  the extra 2048-sample delay at 44.1 kHz.
- Read float32 WAV as floating point rather than signed integers; validate
  formats, extensible subtypes, chunk sizes and non-finite samples.

## Measurements on the supplied recording

Windows x64 Release, MSVC 14.51.36231, 44,100 Hz stereo, 96 kb/s. Source:
the user's local FLAC, 245.533 seconds. Source recordings and generated listening
files are local test material and are not needed by the regression suite.

| Measurement | Before | After |
| --- | ---: | ---: |
| Encode seconds 40–52 of the source | 29.855 s | 2.893 s |
| Encode the entire source | not timed | 59.787 s |
| Full-track SNR, left | 3.57 dB | 10.32 dB |
| Full-track SNR, right | 3.62 dB | 10.42 dB |
| Full-track correlation, left/right | 0.749 / 0.752 | 0.954 / 0.955 |
| Extra output delay | 2048 samples | 0 samples |

The speed comparison used the original executable and the final executable on
the same 12-second PCM input: approximately 10.3 times faster. The full-track
baseline quality is the user's supplied decoded WAV. The final file was also
decoded independently with FFmpeg. The mean-square reconstruction error is
about 4.8 times lower on this recording. SNR is a waveform metric, not a
perceptual score or a listening-test result.

`tools/compare_audio.mjs` aligns the recordings by cross-correlation and reports
the applied delay and polarity. The existing local decoder has opposite global
polarity to FFmpeg; the metric compensates for this explicitly. It does not
normalize gain. Metrics omit 4096 samples at each end and exclude padded tails.
The local 16-bit WAV and FFmpeg float output have correlation above 0.99999 after
polarity alignment; their remaining difference includes 16-bit rounding/clipping.

There are still lossy-codec artifacts: this encoder uses long blocks and does
not yet perform a full PPC pitch search or transient block switching. Decoded
float peaks reach approximately 1.246 on the supplied recording, with fewer
than 0.007% of samples per channel outside full scale. The standalone decoder
clips these when writing its 16-bit listening WAV. This is an improvement in
reconstruction error, not a claim of transparent or lossless audio.

The downloaded FFmpeg build logs an end-of-input demux warning on both the
original and new VQF. It nevertheless decodes the same number of audio frames as
the local decoder. For the final file: 5290 encoded frames, 5288 output hops,
10,829,824 samples/channel, including 1804 trailing pad samples. The packet-size
calculation can be inspected in FFmpeg's
[VQF demuxer](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/vqf.c).

## Regression checks

All checks passed:

- Forward MDCT versus a direct cosine oracle for every power-of-two size
  32–2048, using impulse, tone and noise inputs: maximum error 6.36e-8.
- MDCT overlap-add identity at all these sizes: maximum error 4.77e-7.
- IMDCT versus the existing direct oracle: maximum error 5.41e-10.
- LPC analysis from known stable predictors of orders 8, 12, 16 and 20.
- All 18 legal mono/stereo modes: zero-delay chirp reconstruction, SNR above
  10 dB, gain within 3 dB, and silence peak below 0.001.
- Identical encoded bytes for whole-buffer and irregular chunked feeds,
  including zero-length feeds and repeated flushes; correct final sample count.
- Equivalent 16/24/32-bit integer and float32 WAV inputs, ordinary and
  extensible headers; reject truncated chunks and non-finite float samples.

Commands from the repository root:

```
bin\x64\Release\vqf_encode.exe --test-mdct
bin\x64\Release\vqf_encode.exe --test-codec
bin\x64\Release\vqf_encode.exe --test-roundtrip 0.5
node tools/test_wav.mjs bin/x64/Release/vqf_encode.exe
node tools/compare_audio.mjs reference.wav decoded.wav
```

To build with the installed VS 2026 toolset, including SDK projects:

```
msbuild vqf_encoder.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.51.36231
```

The audio-analysis tools and FFmpeg are used only for verification; neither is
linked into the encoder or the foobar2000 component.
