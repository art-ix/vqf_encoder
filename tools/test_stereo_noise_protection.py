#!/usr/bin/env python3
"""Check stereo-noise-protection activation, disable, bit budget and execution paths."""
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


def main():
    exe = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'bin/vqf_encode').resolve())
    samples = []
    for i in range(22050):
        t = i / 44100
        phase = t % 0.125
        left = sum(math.exp(-phase * (8 + k)) * math.sin(2 * math.pi * 173 * k * t) / k
                   for k in range(1, 40))
        right = sum(math.exp(-phase * (8 + k)) * math.sin(2 * math.pi * 227 * k * t) / k
                    for k in range(1, 32))
        samples.extend((round((left + 0.05 * right) * 4500),
                        round((left - 0.05 * right) * 4500)))
    profiles = [
        ('default', []), ('on', ['--stereo-noise-protection']),
        ('off', ['--no-stereo-noise-protection']),
        ('reset', ['--stereo-noise-protection', '--no-stereo-noise-protection']),
        ('scalar', ['--stereo-noise-protection', '--threads', '1', '--simd', 'scalar']),
        ('parallel', ['--stereo-noise-protection', '--threads', '4']),
    ]
    with tempfile.TemporaryDirectory(prefix='vqf-stereo-') as directory:
        root = Path(directory)
        source = root / 'input.wav'
        with wave.open(str(source), 'wb') as wav:
            wav.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            wav.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
        for bitrate in (80, 96):
            outputs = {}
            for name, flags in profiles:
                dest = root / (name + '.vqf')
                subprocess.run([exe, '-b', str(bitrate), *flags, str(source), str(dest)],
                               check=True, capture_output=True)
                outputs[name] = dest.read_bytes()
            assert outputs['default'] == outputs['on'], 'default must enable protection'
            assert outputs['off'] == outputs['reset'], 'explicit disable must win'
            assert outputs['on'] != outputs['off'], 'harmonic input must exercise protection'
            assert outputs['scalar'] == outputs['parallel'] == outputs['on'], 'execution paths differ'
            assert len({len(data) for data in outputs.values()}) == 1, 'bit budget changed'
    print('stereo noise protection: default, activation, disable, budget and execution paths OK')


if __name__ == '__main__':
    main()
