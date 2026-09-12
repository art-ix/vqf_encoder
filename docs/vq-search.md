# Main-VQ search breadth

The main VQ supports 4, 8, 16 or 32 first-stage candidates. Coordinate refinement
runs after each prefix of four and retains its winner. Widening the beam retains
the four-candidate refined solution for the same target and weights. This
fixed-vector property does not guarantee perceptual improvement on every input.

CLI `--vq-beam auto` is the default. It chooses 16 candidates for 44.1 kHz stereo
(80/96 kbps) and four for other modes. Explicit values 4, 8, 16 and 32 override
it. API `Encoder::Config::vq_beam=0` selects auto. The foobar2000 component uses
the shared configuration. Larger beams require more encoding work but do not
change the mode's bit allocation. LSP and Bark search remain enabled by default.

`tools/test_vq_options.py` uses temporary synthetic PCM to check automatic and
explicit settings, bit budgets and invalid values. It is also invoked by CI.

```sh
python3 tools/test_vq_options.py bin/vqf_encode
vqf_encode --vq-beam 16 -b 96 input.wav output.vqf
```

`tools/benchmark_music.py` is an optional local measurement utility requiring
NumPy and FFmpeg. It requires an explicit source, excerpt start positions and
an output directory outside the repository. It rejects output directories
inside the repository. It does not upload audio or measurements.

```sh
python3 tools/benchmark_music.py /outside/repo/source.m4a --encoder bin/vqf_encode --output-dir /outside/repo/music-test --starts 10 30 60
```

Do not commit private sources, excerpts or their measurement results without
authorization. No recording-derived results are included in this document.
See [block-switching prerequisites](transient-block-implementation.md) for the
separate transform/subblock work required before enabling short frames.


## Final gain refinement

After the bounded gain/VQ iterations, refit each transmitted channel gain to
the final retained codevectors. A last VQ search can change the vectors after
the preceding gain fit. Ending at that point leaves a potentially stale gain.

For fixed vectors `v`, target `x`, weights `w` and gain ratio `r`, the error is
`sum(w * (x - r*v)^2)`. Its continuous minimum is
`max(0, sum(w*x*v) / sum(w*v*v))`. Search all 256 decoder-reconstructed gains
for the nearest available ratio. Retain the preceding frame candidate if the
explicit error calculation does not improve. Do not run another VQ search
after this final fit.

This adds no bits and no additional VQ search. It uses the same objective as
the rest of the encoder, including psychoacoustic weights when enabled. The
retention guarantee applies to this frame candidate with fixed histories;
it does not guarantee improved listening quality or time-domain SNR for every
recording. The selected LSP/Bark histories can still affect later frames.

Validation commands include `--test-codec`, `--test-codec-psychoacoustic` and
`tools/test_vq_options.py`. They cover normal/masking objectives, all supported
mode/channel combinations, chunking/flush behavior and the fixed bit budget.
