#!/usr/bin/env python3
"""Synthetic broadband protection activation and execution-path regression check.

Usage: python tools/test_voice_protection.py [ENCODER]
"""
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import wave


def main():
    exe = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'bin/vqf_encode').resolve())
    profiles = [
        ('off', []),
        ('reset', ['--sibilant-protection', '--no-sibilant-protection']),
        ('scalar', ['--sibilant-protection', '--threads', '1', '--simd', 'scalar']),
        ('auto', ['--sibilant-protection', '--threads', '4']),
    ]
    with tempfile.TemporaryDirectory(prefix='vqf-voice-') as directory:
        root = Path(directory)
        source = root / 'input.wav'
        rng = random.Random(715)
        samples = [rng.randrange(-12000, 12001) for _ in range(16384 * 2)]
        with wave.open(str(source), 'wb') as wav:
            wav.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            wav.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
        outputs = {}
        for name, flags in profiles:
            destination = root / (name + '.vqf')
            subprocess.run([exe, '-b', '96', *flags, str(source), str(destination)],
                           check=True, capture_output=True)
            outputs[name] = destination.read_bytes()
        if outputs['off'] != outputs['reset']:
            raise AssertionError('Explicit disable must restore default output')
        if outputs['scalar'] != outputs['auto']:
            raise AssertionError('Protection must agree across SIMD/thread paths')
        if outputs['off'] == outputs['auto']:
            raise AssertionError('Broadband input must exercise protection')
        if len(outputs['off']) != len(outputs['auto']):
            raise AssertionError('Protection must preserve the bit budget')
        print('voice protection: activation, disable, bit budget and execution paths OK')


if __name__ == '__main__':
    main()
