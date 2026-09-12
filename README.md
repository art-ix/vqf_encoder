# TwinVQ / VQF encoder for foobar2000

Independent TwinVQ encoder for Windows. It writes NTT / Yamaha SoundVQ files
(`.vqf`) from WAV. For [foobar2000](https://www.foobar2000.org/) the encoder is
built into a single component DLL — no `vqf_encode.exe` is required.

The codec implementation does **not** use FFmpeg, `tvqenc.dll`, or the Yamaha
SoundVQ SDK. Bitstream, codebooks and VQF chunks match
[`vqf_decoder`](https://github.com/art-ix/vqf_decoder) (`foo_input_vqf`).

**Grok** (xAI) is a co-author of this library.

## Prebuilt binaries

Download the Windows x64 zip from [Releases](https://github.com/art-ix/vqf_encoder/releases/latest):

* `foo_enc_vqf.dll` — **the only file foobar2000 needs** (encoder is inside the DLL)
* `vqf_encode.exe` / `vqf_decode.exe` — optional standalone CLI, not used by the component

## Features

* Encode TwinVQ / VQF (the proprietary NTT bitstream, not MPEG-4 TwinVQ)
* Modes from 8 kHz / 8 kbit/s/ch up to 44.1 kHz / 48 kbit/s/ch
* Write VQF metadata (NAME, AUTH, COMT, (c), ALBM, GENR, TRCK, YEAR, MUSC, LABL)
* foobar2000 2.x: native encoder in `foo_enc_vqf.dll` (Convert → TwinVQ / VQF)
* Optional CLI tools for encode/decode outside foobar2000

## Layout

```
twinvq/            TwinVQ library (encoder + decoder, no foobar2000 dependency)
  include/twinvq/  Public headers
  src/             Encoder, decoder, parser, MDCT, codebooks
foo_enc_vqf/       foobar2000 encoder component
tools/             vqf_encode / vqf_decode CLI
audio/             Sample .vqf files
sdk/               foobar2000 SDK — not in git; you add this locally
```

The foobar2000 SDK is **not** part of this repository.

## Requirements

* Windows x64
* Visual Studio 2022 or later, with the C++ x64 toolset and a Windows 10/11 SDK
* foobar2000 2.x (x64)
* [foobar2000 SDK](https://www.foobar2000.org/SDK) only if you build `foo_enc_vqf`

The `twinvq` library and `vqf_encode` CLI build without the SDK.

Linux (this tree): `make -j && make test` (g++ 12+, C++17). Objects live in
`obj/`; codebook tables compile at `-O0` so incremental encoder rebuilds skip
the 575 KB table TU.

## foobar2000 SDK (component build)

1. Download the SDK from https://www.foobar2000.org/SDK
2. Extract it so the tree looks like this:

```
sdk/
  foobar2000/
    SDK/
    shared/
    foobar2000_component_client/
  pfc/
```

## Build

From the repository root, in a Visual Studio developer prompt:

```
msbuild vqf_encoder.sln /p:Configuration=Release /p:Platform=x64
```

If the SDK projects still ask for toolset v142, add `/p:PlatformToolset=v143`
(VS 2022) or `v145` (VS 2026).

Outputs:

| File | Purpose |
| --- | --- |
| bin\x64\Release\vqf_encode.exe | Command-line encoder |
| bin\x64\Release\vqf_decode.exe | Command-line decoder (roundtrip tests) |
| bin\x64\Release\foo_enc_vqf.dll | foobar2000 encoder component |

Encoder-only (no SDK):

```
msbuild twinvq\twinvq.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild tools\vqf_encode.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Install the foobar2000 component

Copy **only** `foo_enc_vqf.dll` into the foobar2000 **components** folder, then
restart. Do not copy `vqf_encode.exe` or `vqf_decode.exe` there.

```
C:\Program Files\foobar2000\components\foo_enc_vqf.dll
```

The TwinVQ encoder (codebooks, MDCT, bitstream) is linked into that DLL.

Keep `foo_input_vqf.dll` installed if you want to play the files you encode.

Check **File → Preferences → Components** for “TwinVQ encoder”.
Bitrate is under **Preferences → Advanced → TwinVQ encoder**.

## Convert

Right-click tracks → **Convert → TwinVQ / VQF**. A settings dialog asks for
bitrate (total kbps) and destination (same folder as the source, like Quick
convert, or a folder you pick). Encoding runs inside the DLL.

foobar2000’s **Quick convert** list and **Convert → …** Output format dropdown
are owned by `foo_converter` and only show built-in PCM formats plus
command-line encoder presets. A native component cannot add a row there
without an external EXE. Use **Convert → TwinVQ / VQF** instead.

## Standalone CLI (optional)

`vqf_encode.exe` / `vqf_decode.exe` are for use outside foobar2000. They are
not required by the component.

Bitrate `-b` is **total** kbps. Stereo 44.1 kHz uses **80 or 96 kbps**
(40 or 48 kbps/ch). The encoder snaps to the nearest legal TwinVQ mode.

There is **no 128 kbps** VQF mode. The COMM chunk can store any integer, but
the bitstream is looked up as `(sample_rate_kHz, kbps_per_channel)` against a
fixed set of NTT codebooks. FFmpeg, Yamaha SoundVQ, `vqf_decoder` and this
encoder all stop at **48 kbps/ch**. Wikipedia’s 112–192 kbit/s list does not
match released TwinVQ tables.

```
vqf_encode.exe --list-modes
vqf_encode.exe -b 96 --title "Track" --artist "Name" input.wav out.vqf
vqf_encode.exe --test-mdct
vqf_encode.exe --test-codec
vqf_encode.exe --test-roundtrip 0.5
```


## Supported modes

| Rate | kbps / ch | Frame |
| --- | --- | --- |
| 8 kHz | 8 | 512 |
| 11.025 kHz | 8, 10 | 512 |
| 16 kHz | 16 | 1024 |
| 22.05 kHz | 20, 24, 32 | 1024 / 512 |
| 44.1 kHz | 40, 48 | 2048 |

**128 kbps stereo is not possible** — that would be 64 kbps/ch, and there is
no `44_64` codebook. Highest 44.1 kHz stereo mode is 96 kbps.


Input WAV is 16/24/32-bit PCM or 32-bit IEEE float, mono or stereo, including
WAVE_FORMAT_EXTENSIBLE. Other rates are converted to the selected TwinVQ rate using a centered
Blackman-windowed sinc filter with anti-alias filtering. The transition band
is approximately 90–100% of the lower Nyquist frequency. Conversion preserves
sample-zero alignment, rounds output duration to the nearest sample, and
extends endpoint samples at the boundaries. Native-rate input is unchanged.
Run `vqf_encode --test-resample` for filter and timing regression checks.

The encoder uses an FFT-based forward MDCT, reconstruction-weighted two-stage
VQ searches, and fitted channel gains. See [quality measurements and regression
checks](docs/encoder-quality.md) for the September 2026 fixes and their limits.

`--test-mdct` checks forward/inverse transforms, overlap-add and LPC analysis.
`--test-codec` checks all 18 mono/stereo modes, signal quality, gain, silence,
priming, tail flushing and equivalence between whole-buffer and chunked input.
Optional WAV-reader regression checks require Node.js:

```
node tools/test_wav.mjs bin/x64/Release/vqf_encode.exe
node tools/compare_audio.mjs reference.wav decoded.wav
```

## Authors

* Artur Pełzak
* Grok (xAI) — co-author of the TwinVQ encoder, foobar2000 component, and tooling

## License

This project’s original source is MIT. See [LICENSE](LICENSE).

The foobar2000 SDK (not included) is copyright Peter Pawlowski and is licensed separately.

Codebook tables are numerical TwinVQ constants required to encode the format.

Known NTT TwinVQ patent families expired around 2015–2020. That is not legal advice.


### Experimental LSP search

`vqf_encode --lsp-search -b 96 input.wav output.vqf` enables prediction-aware
LSP beam search, comparing the ordinary and searched candidates after full
frame quantization. It is disabled by default because some test signals regress
and encoding takes approximately twice as long. In the C++ API set
`Encoder::Config::lsp_search = true`. The foobar2000 component keeps the default.
Run `vqf_encode --test-codec-lsp` to check all 18 modes with this option.
See [the audit](docs/encoder-quality-audit.md) for measured gains and regressions.


### Experimental Bark/history search

`vqf_encode --bark-search -b 96 input.wav output.vqf` evaluates an additional
Bark candidate using LPC synthesis weights and both history settings. It is
opt-in because music listening validation is still pending and encoding costs
roughly twice as much. In the C++ API set `Encoder::Config::bark_search = true`.
Combine with `--lsp-search` to evaluate all four ordinary/searched LSP/Bark
combinations per frame, at a correspondingly higher cost. The selected frame's
parameters and histories are committed together.

`--test-codec-bark` tests Bark search in all 18 modes; `--test-codec-search`
tests both options together. These tests also check that Bark history flags
are actually transmitted. Default encoding and the foobar2000 component remain
unchanged. Measurements and limitations are in [the audit](docs/encoder-quality-audit.md).
