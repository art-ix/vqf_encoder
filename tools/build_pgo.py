#!/usr/bin/env python3
"""Build an optional GCC PGO encoder, trained only on generated PCM.

All objects/profiles live in a temporary directory. The output is copied only
when complete-stream equivalence and SIMD/thread checks pass.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import math
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd):
    subprocess.run([str(x) for x in args], cwd=cwd, check=True)


def train(exe, directory):
    cases = [
        (16000, 1, 16, ['--simd', 'scalar', '--no-lsp-search']),
        (44100, 2, 80, ['--simd', 'scalar', '--vq-beam', '4']),
        (44100, 2, 80, []),
        (44100, 2, 96, ['--vq-beam', '32', '--sibilant-protection']),
        (44100, 2, 80, ['--sibilant-protection', '--psychoacoustic',
                         '--block-mode', 'adaptive', '--temporal-search']),
        (44100, 2, 96, ['--block-mode', 'short']),
        (44100, 2, 80, ['--block-mode', 'medium', '--psychoacoustic']),
    ]
    for index, (rate, channels, bitrate, flags) in enumerate(cases):
        rng = random.Random(1927 + index)
        samples = []
        for i in range(10 * 2048 + 29):
            t = i / rate
            voiced = sum(math.sin(2 * math.pi * 137 * h * t) / h for h in range(1, 8))
            noise = rng.uniform(-1, 1) if 4096 <= i < 6144 or 14336 <= i < 16384 else 0
            for ch in range(channels):
                value = 2200 * voiced + 6500 * noise * (1 if ch == 0 else -0.6)
                samples.append(round(value))
        source, output = directory / 'training.wav', directory / 'training.vqf'
        with wave.open(str(source), 'wb') as wav:
            wav.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
            wav.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
        run([exe, '-b', bitrate, '--threads', '2', *flags, source, output], directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++', help='GCC compiler executable (GCC 12 or newer)')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--output', type=Path, default=ROOT / 'bin/vqf_encode_pgo')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    version = subprocess.check_output([args.cxx, '-dumpfullversion'], text=True).strip()
    if int(version.split('.')[0]) < 12:
        parser.error('GCC 12 or newer is required')
    print('Building with GCC', version, flush=True)
    flags = ['-pthread', '-O2', '-std=c++17', '-Wall', '-Wno-unused-function', '-pipe']
    includes = ['-I' + str(ROOT / 'twinvq/include'), '-I' + str(ROOT / 'twinvq/src')]
    sources = [ROOT / 'twinvq/src' / (name + '.cpp') for name in
               ['vqf_file', 'twinvq_mdct', 'twinvq_decoder', 'twinvq_encoder',
                'twinvq_window_test', 'twinvq_psychoacoustic', 'twinvq_tables']]
    sources.append(ROOT / 'tools/vqf_encode.cpp')
    with tempfile.TemporaryDirectory(prefix='vqf-pgo-') as temporary:
        directory = Path(temporary)
        objects = [directory / (src.stem + '.o') for src in sources]

        def compile_one(pair):
            src, obj = pair
            options = flags if src.stem != 'twinvq_tables' else ['-O0', '-std=c++17']
            run([args.cxx, *options, *includes, '-c', src, '-o', obj], directory)

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            list(pool.map(compile_one, zip(sources, objects)))
        baseline = directory / 'baseline'
        run([args.cxx, *flags, '-o', baseline, *objects], directory)
        encoder_source = ROOT / 'twinvq/src/twinvq_encoder.cpp'
        encoder_object = directory / 'twinvq_encoder.o'
        profile = directory / 'profiles'
        generate = ['-fprofile-generate=' + str(profile), '-fprofile-update=atomic']
        run([args.cxx, *flags, *generate, *includes, '-c', encoder_source,
             '-o', encoder_object], directory)
        instrumented = directory / 'instrumented'
        run([args.cxx, *flags, *generate, '-o', instrumented, *objects], directory)
        train(instrumented, directory)
        if not list(profile.rglob('*.gcda')):
            raise RuntimeError('Training did not generate profile data')
        use = ['-fprofile-use=' + str(profile), '-fprofile-partial-training',
               '-Werror=missing-profile', '-Werror=coverage-mismatch']
        # Keep the same object path for profile lookup. Only this translation
        # unit is profiled; the remaining objects keep ordinary -O2 behavior.
        run([args.cxx, *flags, *use, *includes, '-c', encoder_source,
             '-o', encoder_object], directory)
        candidate = directory / 'candidate'
        run([args.cxx, *flags, '-o', candidate, *objects], directory)
        run(['python3', ROOT / 'tools/test_encoder_equivalence.py', candidate, baseline], directory)
        run(['python3', ROOT / 'tools/test_voice_protection.py', candidate], directory)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(candidate, args.output)
        print('Validated PGO encoder:', args.output.resolve(), flush=True)


if __name__ == '__main__':
    main()
