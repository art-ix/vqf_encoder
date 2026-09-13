#!/usr/bin/env python3
"""Synthetic bitstream equivalence for SIMD and VQ workers.
Usage: test_parallel_options.py ENCODER [PREVIOUS_ENCODER]
"""
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import time
import wave

exe = str(Path(sys.argv[1]).resolve())
previous = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
subprocess.run([exe, '--test-workers'], check=True, capture_output=True)
caps = subprocess.run([exe, '--test-simd'], check=True, capture_output=True, text=True).stdout
backends = ['scalar'] + [name for name in ('sse41', 'avx2') if name + '=1' in caps]
cases = [
    (44100, 2, 80, ['--ppc-search']),
    (44100, 2, 96, ['--block-mode', 'adaptive', '--temporal-search', '--ppc-search', '--psychoacoustic']),
    (44100, 2, 80, ['--block-mode', 'medium', '--vq-beam', '32']),
    (16000, 1, 16, ['--ppc-search', '--vq-beam', '4']),
]
with tempfile.TemporaryDirectory(prefix='vqf-parallel-') as directory:
    root = Path(directory)
    for rate, channels, bitrate, flags in cases:
        rng = random.Random(123)
        pcm = []
        for i in range(8193):
            for ch in range(channels):
                attack = 6500 * rng.uniform(-1, 1) if 4800 <= i < 5500 else 0
                pcm.append(round(2300 * math.sin(i * (0.083 if ch == 0 else 0.097)) + attack))
        source, output = root / 'input.wav', root / 'out.vqf'
        with wave.open(str(source), 'wb') as wav:
            wav.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
            wav.writeframes(struct.pack('<' + 'h' * len(pcm), *pcm))
        reference = None
        if previous:
            subprocess.run([previous, '-b', str(bitrate), *flags, str(source), str(output)], check=True, capture_output=True)
            reference = output.read_bytes()
        for backend, threads in [(b, t) for b in backends for t in (1, 4)] + [('auto', 4), ('auto', 'auto'), ('auto', None)]:
            start = time.perf_counter()
            worker_flags = [] if threads is None else ['--threads', str(threads)]
            subprocess.run([exe, '-b', str(bitrate), *flags, '--simd', backend, *worker_flags,
                            str(source), str(output)], check=True, capture_output=True)
            data = output.read_bytes()
            if reference is None:
                reference = data
            assert data == reference, (rate, channels, bitrate, flags, backend, threads)
            print(rate, channels, bitrate, backend, threads, 'identical', round(time.perf_counter() - start, 3), flush=True)
    for flags in (['--threads', '0'], ['--threads', '33'], ['--threads', '2junk'], ['--threads'], ['--simd', 'bad'], ['--simd']):
        result = subprocess.run([exe, *flags], capture_output=True)
        assert result.returncode != 0, flags
print('Parallel/SIMD options passed', flush=True)
