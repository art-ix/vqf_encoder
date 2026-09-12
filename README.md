# TwinVQ / VQF encoder for foobar2000

Independent TwinVQ encoder for Windows. It writes NTT / Yamaha SoundVQ files
(`.vqf`) from WAV, and registers a native encoder plus a Converter-friendly
CLI for [foobar2000](https://www.foobar2000.org/).

The codec implementation does **not** use FFmpeg, `tvqenc.dll`, or the Yamaha
SoundVQ SDK. Bitstream, codebooks and VQF chunks match
[`vqf_decoder`](https://github.com/art-ix/vqf_decoder) (`foo_input_vqf`).

**Grok** (xAI) is a co-author of this library.

## Prebuilt binaries

Download the Windows x64 zip from [Releases](https://github.com/art-ix/vqf_encoder/releases/latest):

* `foo_enc_vqf.dll` — native foobar2000 2.x encoder component
* `vqf_encode.exe` — Converter / CLI encoder
* `vqf_decode.exe` — CLI decoder for roundtrip tests

## Features

* Encode TwinVQ / VQF (the proprietary NTT bitstream, not MPEG-4 TwinVQ)
* Modes from 8 kHz / 8 kbit/s/ch up to 44.1 kHz / 48 kbit/s/ch
* Write VQF metadata (NAME, AUTH, COMT, (c), ALBM, GENR, TRCK, YEAR, MUSC, LABL)
* `vqf_encode.exe` CLI for foobar2000 Converter
* Native `fb2k::audioEncoder` service (foobar2000 2.x)

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

Copy `foo_enc_vqf.dll` into the foobar2000 **components** folder, then restart.

```
C:\Program Files\foobar2000\components\foo_enc_vqf.dll
```

Keep `foo_input_vqf.dll` installed if you want to play the files you encode.

Check **File → Preferences → Components** for “TwinVQ encoder”.
Bitrate is under **Preferences → Advanced → TwinVQ encoder**.

## Converter (CLI)

Copy `vqf_encode.exe` next to foobar2000 (or into `encoders\`). Add a custom
Converter preset:

| Field | Value |
| --- | --- |
| Encoder | `vqf_encode.exe` |
| Extension | `vqf` |
| Parameters | `-b 96 %s %d` |
| Format | WAV (temp file, `%s`) |

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


Input WAV is 16/24/32-bit PCM, mono or stereo. Other rates are linearly
resampled to the nearest TwinVQ rate.

## Authors

* Artur Pełzak
* Grok (xAI) — co-author of the TwinVQ encoder, foobar2000 component, and tooling

## License

This project’s original source is MIT. See [LICENSE](LICENSE).

The foobar2000 SDK (not included) is copyright Peter Pawlowski and is licensed separately.

Codebook tables are numerical TwinVQ constants required to encode the format.

Known NTT TwinVQ patent families expired around 2015–2020. That is not legal advice.
