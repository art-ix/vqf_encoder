#!/usr/bin/env python3
"""Compare two encoder executables on deterministic synthetic 44.1 kHz audio.
Requires ffmpeg on PATH. Usage: benchmark_gain.py baseline candidate [output-dir] [candidate-options...]
Waveform SNR is diagnostic, not a perceptual score. Generated files are temporary.
"""
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import time
import wave

baseline, candidate = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
root = Path(sys.argv[3] if len(sys.argv) > 3 else "obj/gain-benchmark")
root.mkdir(parents=True, exist_ok=True)
rate, frames = 44100, 44100 * 2
rng = random.Random(12345)
results = []
for name in ("tones", "harmonics", "attacks", "noise", "fade", "identical", "antiphase", "left-only"):
    pcm = []
    for i in range(frames):
        t = i / rate
        l = 0.2 * math.sin(2*math.pi*437*t) + 0.06 * math.sin(2*math.pi*1123*t)
        r = 0.2 * math.sin(2*math.pi*653*t) + 0.06 * math.sin(2*math.pi*1379*t)
        if name == "harmonics":
            l = sum(0.18/k*math.sin(2*math.pi*173*k*t) for k in range(1, 18))
            r = sum(0.18/k*math.sin(2*math.pi*227*k*t) for k in range(1, 18))
        elif name == "attacks":
            envelope = math.exp(-40*(t % 0.2))
            l, r = envelope*(l+0.1*rng.uniform(-1,1)), envelope*(r+0.1*rng.uniform(-1,1))
        elif name == "noise":
            l, r = 0.2*rng.uniform(-1,1), 0.2*rng.uniform(-1,1)
        elif name == "fade":
            l, r = l*10**(-2*t), r*10**(-2*t)
        elif name == "identical": r = l
        elif name == "antiphase": r = -l
        elif name == "left-only": r = 0
        pcm.extend((round(l*32767), round(r*32767)))
    source = root / (name + ".wav")
    with wave.open(str(source), "wb") as w:
        w.setparams((2, 2, rate, 0, "NONE", "not compressed"))
        w.writeframes(struct.pack("<"+"h"*len(pcm), *pcm))
    row = {"signal": name}
    for label, exe in (("before",baseline), ("after",candidate)):
        encoded = root / (name+"-"+label+".vqf")
        decoded = encoded.with_suffix(".f32")
        start = time.perf_counter()
        subprocess.run([exe, *(sys.argv[4:] if label == "after" else []), str(source), str(encoded)], check=True, capture_output=True)
        elapsed = time.perf_counter()-start
        run = subprocess.run(["ffmpeg","-v","error","-y","-i",str(encoded),"-f","f32le",str(decoded)],
                             check=True, capture_output=True, text=True)
        raw = decoded.read_bytes()
        values = struct.unpack("<"+"f"*(len(raw)//4), raw)
        if len(values) < len(pcm) or not all(map(math.isfinite,values)):
            raise RuntimeError("invalid decoded PCM")
        # Known opposite global polarity in the current independent decoder.
        # Zero lag is deliberate: do not conceal alignment regressions.
        signal = error = 0.0
        for i in range(4096*2, (frames-4096)*2):
            x, y = pcm[i]/32768, -values[i]
            signal += x*x
            error += (x-y)**2
        row[label] = {"snr_db":10*math.log10(signal/error), "seconds":elapsed,
                      "bytes":encoded.stat().st_size, "decoded_frames":len(values)//2,
                      "peak":max(map(abs, values)), "demux_warning":bool(run.stderr)}
    if row["before"]["bytes"] != row["after"]["bytes"] or row["before"]["decoded_frames"] != row["after"]["decoded_frames"]:
        raise RuntimeError("bit budget or decoded duration changed")
    row["snr_delta_db"] = row["after"]["snr_db"] - row["before"]["snr_db"]
    results.append(row)
    print(json.dumps(row), flush=True)
(root / "results.json").write_text(json.dumps(results, indent=2)+"\n")
