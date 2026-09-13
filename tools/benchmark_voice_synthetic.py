#!/usr/bin/env python3
"""Deterministic synthetic voice/fricative diagnostic for 44.1 kHz stereo VQF.

This is a metric benchmark, not a listening test and not a Polish phoneme
recognizer.  It deliberately avoids private recordings.  The generated signal
contains a harmonic/formant-like voice bed plus repeatable broadband bursts
chosen to stress the same 2.5--9 kHz region as --sibilant-protection.

Usage:
  python3 tools/benchmark_voice_synthetic.py bin/vqf_encode [bin/vqf_decode]
"""
from collections import deque
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 44100
CHANNELS = 2
DURATION = 1.45
BURSTS = ((0.38, 0.55), (0.83, 0.90), (1.08, 1.25))
TRIM = 4096


def formant_gain(hz):
    peaks = ((650.0, 170.0, 1.0), (1250.0, 240.0, 0.72),
             (2450.0, 330.0, 0.52), (3300.0, 450.0, 0.22))
    value = 0.055
    for center, width, gain in peaks:
        value += gain * math.exp(-0.5 * ((hz - center) / width) ** 2)
    return value


def make_signal():
    frames = int(RATE * DURATION)
    phases = [2.0 * math.pi * i / 29.0 for i in range(1, 30)]
    freqs = [2600.0 + i * (6200.0 / 28.0) for i in range(29)]
    out = []
    for i in range(frames):
        t = i / RATE
        f0 = 148.0 + 7.0 * math.sin(2.0 * math.pi * 2.1 * t)
        voice = 0.0
        for harmonic in range(1, 39):
            hz = harmonic * f0
            if hz >= RATE * 0.48:
                break
            voice += (formant_gain(hz) / harmonic) * math.sin(2.0 * math.pi * hz * t)
        # Short closure before the middle burst approximates an affricate onset.
        closure = 0.22 if 0.795 <= t < 0.83 else 1.0
        voice *= 0.34 * closure
        env = 0.0
        for start, end in BURSTS:
            if start <= t < end:
                edge = min((t - start) / 0.012, (end - t) / 0.015, 1.0)
                env = max(env, max(0.0, edge))
        noise = 0.0
        if env:
            # Dense deterministic high-band components provide a repeatable,
            # broadband stress signal without embedding recorded speech.
            for k, hz in enumerate(freqs):
                noise += math.sin(2.0 * math.pi * hz * t + phases[k])
            noise *= 0.0105 * env
        left = max(-0.98, min(0.98, voice + noise))
        right = max(-0.98, min(0.98, 0.97 * voice + 0.91 * noise))
        out.extend((int(round(left * 32767)), int(round(right * 32767))))
    return out


def write_wav(path, samples):
    with wave.open(str(path), 'wb') as wav:
        wav.setparams((CHANNELS, 2, RATE, 0, 'NONE', 'not compressed'))
        wav.writeframes(struct.pack('<' + 'h' * len(samples), *samples))


def read_wav(path):
    with wave.open(str(path), 'rb') as wav:
        if wav.getnchannels() != CHANNELS or wav.getframerate() != RATE or wav.getsampwidth() != 2:
            raise RuntimeError('unexpected decoded WAV format')
        raw = wav.readframes(wav.getnframes())
    ints = struct.unpack('<' + 'h' * (len(raw) // 2), raw)
    return [[ints[2 * i] / 32768.0, ints[2 * i + 1] / 32768.0]
            for i in range(len(ints) // 2)]


def lowpass(cutoff, taps):
    mid = (taps - 1) / 2.0
    fc = cutoff / RATE
    coeff = []
    for i in range(taps):
        x = i - mid
        ideal = 2.0 * fc if x == 0 else math.sin(2.0 * math.pi * fc * x) / (math.pi * x)
        window = 0.54 - 0.46 * math.cos(2.0 * math.pi * i / (taps - 1))
        coeff.append(ideal * window)
    scale = sum(coeff)
    return [x / scale for x in coeff]


def bandpass(low, high, taps=63):
    hi = lowpass(high, taps)
    lo = lowpass(low, taps)
    return [a - b for a, b in zip(hi, lo)]


def filtered_energy(samples, coeff, predicate):
    history = [deque([0.0] * len(coeff), maxlen=len(coeff)) for _ in range(CHANNELS)]
    energy = 0.0
    count = 0
    for i, frame in enumerate(samples):
        for ch in range(CHANNELS):
            history[ch].appendleft(frame[ch])
        if not predicate(i):
            continue
        for ch in range(CHANNELS):
            y = sum(c * x for c, x in zip(coeff, history[ch]))
            energy += y * y
            count += 1
    return energy, count


def metrics(reference, decoded):
    usable = min(len(reference), len(decoded))
    if usable <= 2 * TRIM:
        raise RuntimeError('decoded signal too short')
    ref = reference[TRIM:usable - TRIM]
    out = decoded[TRIM:usable - TRIM]
    err = [[r[0] - d[0], r[1] - d[1]] for r, d in zip(ref, out)]

    def in_burst(relative_index):
        absolute_t = (relative_index + TRIM) / RATE
        return any(start <= absolute_t < end for start, end in BURSTS)

    voice_filter = bandpass(150.0, 3500.0)
    sib_filter = bandpass(2500.0, 9000.0)
    voice_ref, _ = filtered_energy(ref, voice_filter, lambda i: not in_burst(i))
    voice_err, _ = filtered_energy(err, voice_filter, lambda i: not in_burst(i))
    sib_ref, _ = filtered_energy(ref, sib_filter, in_burst)
    sib_err, _ = filtered_energy(err, sib_filter, in_burst)
    total_ref = sum(x * x for frame in ref for x in frame)
    total_err = sum((r - d) * (r - d) for rf, df in zip(ref, out) for r, d in zip(rf, df))

    def snr(signal, error):
        return 10.0 * math.log10((signal + 1e-30) / (error + 1e-30))

    return {
        'overall_snr_db': snr(total_ref, total_err),
        'voice_band_snr_db': snr(voice_ref, voice_err),
        'sibilant_band_snr_db': snr(sib_ref, sib_err),
    }


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    encoder = Path(sys.argv[1]).resolve()
    decoder = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else encoder.with_name('vqf_decode')
    samples = make_signal()
    reference = [[samples[2 * i] / 32768.0, samples[2 * i + 1] / 32768.0]
                 for i in range(len(samples) // 2)]
    rows = []
    with tempfile.TemporaryDirectory(prefix='vqf-synthetic-voice-') as directory:
        root = Path(directory)
        source = root / 'voice-fricatives.wav'
        write_wav(source, samples)
        for bitrate in (80, 96):
            for protection in (False, True):
                encoded = root / f'{bitrate}-prot{int(protection)}.vqf'
                decoded_path = root / f'{bitrate}-prot{int(protection)}.wav'
                flags = ['--vq-beam', '32', '--sibilant-protection' if protection else '--no-sibilant-protection']
                subprocess.run([str(encoder), '-b', str(bitrate), *flags, str(source), str(encoded)],
                               check=True, capture_output=True)
                subprocess.run([str(decoder), str(encoded), str(decoded_path)],
                               check=True, capture_output=True)
                row = {'kbps': bitrate, 'beam': 32, 'sibilant_protection': protection,
                       **metrics(reference, read_wav(decoded_path))}
                rows.append(row)
                print(json.dumps(row, sort_keys=True), flush=True)
        for bitrate in (80, 96):
            off = next(r for r in rows if r['kbps'] == bitrate and not r['sibilant_protection'])
            on = next(r for r in rows if r['kbps'] == bitrate and r['sibilant_protection'])
            delta = {
                'kbps': bitrate,
                'delta_overall_snr_db': on['overall_snr_db'] - off['overall_snr_db'],
                'delta_voice_band_snr_db': on['voice_band_snr_db'] - off['voice_band_snr_db'],
                'delta_sibilant_band_snr_db': on['sibilant_band_snr_db'] - off['sibilant_band_snr_db'],
            }
            print(json.dumps(delta, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
