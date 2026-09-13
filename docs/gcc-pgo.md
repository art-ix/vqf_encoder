# Optional GCC profile-guided encoder build

```sh
python3 tools/build_pgo.py --jobs 2
# Result: bin/vqf_encode_pgo
```

The script requires Python 3 and GCC 12 or newer (validated with GCC 13.3).
`--cxx` accepts a compiler executable, and `--output` selects the destination.
This is a separate Linux/GCC build; normal Makefile builds and Windows release
binaries do not automatically enable PGO. It does not use LTO or fast-math,
and it does not enable new codec quality settings.

## Reproducible process

1. Compile the regular encoder with the project's ordinary `-O2` flags in a
   temporary directory. Codebook tables are compiled at `-O0`.
2. Recompile only `twinvq_encoder.cpp` with `-fprofile-generate` and atomic
   profile updates, then link an instrumented encoder. This translation unit
   includes the inline SIMD search kernels. Other objects remain unprofiled.
3. Generate deterministic harmonic/noise/burst PCM for seven training cases:
   mono, stereo 80/96, scalar/automatic SIMD, short/medium/adaptive blocks,
   masking, temporal ranking and optional voice protection. Training uses two
   workers. No uploaded recordings are read, copied or published.
4. Recompile the encoder at the same object path with `-fprofile-use` and
   `-fprofile-partial-training`. Missing or mismatched profiles are errors.
5. Compare six independent synthetic streams with the unprofiled build byte
   for byte, then run the voice protection SIMD/thread execution-path test.
   Only a passing candidate is copied to the output path.

Objects, profiles and generated training audio are temporary and removed when
the script exits. A failed build does not replace an existing output binary.
The compiler flags are deliberately defined in this script; environment
`CXXFLAGS` and custom Makefile overrides are not inherited. For meaningful
comparisons, use matching compilers and floating-point settings.

## Limits

Training covers representative paths, not every supported mode or processor.
Byte equality on these checks does not guarantee equality across compilers,
platforms or arbitrary audio. Measure performance with the actual deployment
CPU and relevant settings. The generated profile is specific to this source
and compiler; rerun the build after code changes instead of reusing old data.

The regular output remains available as `bin/vqf_encode`, so comparisons can
use the same CLI parameters. PGO changes compiler decisions; it is not a new
speech model and cannot by itself establish a listening-quality improvement.
The script does not change the default beam size, PPC or voice-protection flag.
