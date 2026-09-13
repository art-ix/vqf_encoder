#!/usr/bin/env python3
"""Compare optimized/reference encoders on synthetic PCM, byte for byte.

Usage: python tools/test_encoder_equivalence.py CANDIDATE REFERENCE
Build both binaries with the same compiler and floating-point options.
Timings are single runs, intended as a local smoke benchmark.
"""
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import time
import wave


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    candidate, reference = (str(Path(arg).resolve()) for arg in sys.argv[1:])
    cases = [
        ('mono-basic-ppc', 16000, 1, 16,
         ['--vq-beam', '4', '--ppc-search', '--no-lsp-search', '--no-bark-search']),
        ('stereo-default', 44100, 2, 80, []),
        ('medium-mask', 44100, 2, 80,
         ['--vq-beam', '32', '--psychoacoustic', '--block-mode', 'medium']),
        ('adaptive-time-ppc', 44100, 2, 96,
         ['--vq-beam', '8', '--psychoacoustic', '--ppc-search',
          '--block-mode', 'adaptive', '--temporal-search']),
    ]
    with tempfile.TemporaryDirectory(prefix='vqf-equivalence-') as directory:
        root = Path(directory)
        for name, rate, channels, bitrate, flags in cases:
            rng = random.Random(817)
            samples = []
            for i in range(8 * 2048 + 17):
                tone = 4500 * math.sin(0.083 * i) if i < 3 * 2048 else 0
                attack = 8500 * rng.uniform(-1, 1) if 5 * 2048 + 1024 <= i < 6 * 2048 else 0
                for ch in range(channels):
                    samples.append(round(tone + (attack if ch == 0 else -attack)))
            source = root / 'input.wav'
            with wave.open(str(source), 'wb') as wav:
                wav.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
                wav.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
            outputs, seconds = {}, {}
            for label, exe in [('reference', reference), ('candidate', candidate)]:
                output = root / (label + '.vqf')
                start = time.perf_counter()
                subprocess.run([exe, '-b', str(bitrate), *flags, str(source), str(output)],
                               check=True, capture_output=True)
                seconds[label] = time.perf_counter() - start
                outputs[label] = output.read_bytes()
            if outputs['candidate'] != outputs['reference']:
                raise AssertionError(name + ': encoded bytes changed')
            print(json.dumps(dict(case=name, identical=True, seconds=seconds)), flush=True)


if __name__ == '__main__':
    main()
