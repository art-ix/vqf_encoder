#!/usr/bin/env python3
"""CLI search/default regression checks using temporary synthetic PCM only."""
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import wave

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='vqf-vq-options-') as directory:
    root = Path(directory)
    for channels in (1,2):
        rng = random.Random(42)
        samples = [int(7000*math.sin(i*0.13)+2000*rng.uniform(-1,1))
                   for i in range(8193*channels)]
        source = root/'input.wav'
        with wave.open(str(source),'wb') as w:
            w.setparams((channels,2,44100,0,'NONE','not compressed'))
            w.writeframes(struct.pack('<'+'h'*len(samples),*samples))
        for bitrate in ([48] if channels == 1 else [80,96]):
            outputs = {}
            for option in (None,'auto','4','8','16','32'):
                flags = [] if option is None else ['--vq-beam',option]
                output = root/'out.vqf'
                subprocess.run([exe,'-b',str(bitrate),*flags,str(source),str(output)],
                               check=True,capture_output=True)
                outputs[option] = output.read_bytes()
            expected = '16' if channels == 2 else '4'
            assert outputs[None] == outputs['auto'] == outputs[expected], 'incorrect automatic beam'
            assert len({len(x) for x in outputs.values()}) == 1, 'beam changed bit budget'
            if channels == 2:
                assert outputs['4'] != outputs['16'], 'fixture did not exercise wider search'
            print(channels,bitrate,'beam choices and defaults passed',flush=True)
        for invalid in ('0','3','7','64','8junk'):
            r=subprocess.run([exe,'--vq-beam',invalid,str(source),str(root/'bad.vqf')],
                             capture_output=True,text=True)
            assert r.returncode != 0 and 'VQ beam must' in r.stderr, invalid
print('VQ options tests passed')
