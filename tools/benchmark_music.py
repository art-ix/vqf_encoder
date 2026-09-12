#!/usr/bin/env python3
"""Private music benchmark. Audio/output directory MUST be outside the repository.
Requires FFmpeg and NumPy. Never copies source audio into version control.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('--encoder', type=Path, required=True)
p.add_argument('--output-dir', type=Path, required=True)
p.add_argument('--starts', type=float, nargs='+', required=True)
p.add_argument('--seconds', type=float, default=4)
p.add_argument('--beams', type=int, nargs='+', default=[4,8,16])
p.add_argument('--bitrates', type=int, nargs='+', default=[80,96])
p.add_argument('--masking', choices=['off','on'], nargs='+', default=['off'])
a = p.parse_args()
root = a.output_dir.resolve()
repo = Path(__file__).resolve().parents[1]
if root == repo or repo in root.parents:
    p.error('audio output must be outside the repository')
root.mkdir(parents=True, exist_ok=True)
exe = str(a.encoder.resolve())
probe = json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','a:0',
    '-show_entries','stream=codec_name,sample_rate,channels','-of','json',str(a.source)]))
stream = probe['streams'][0]
if int(stream['sample_rate']) != 44100 or stream['channels'] != 2:
    p.error('this 80/96 kbps benchmark requires 44.1 kHz stereo input')
results = {'source_codec': stream['codec_name'], 'sample_rate':44100, 'channels':2,
           'seconds':a.seconds, 'rows':[]}
for start in a.starts:
    stem = f'clip-{start:g}'
    wav = root/(stem+'.wav')
    subprocess.run(['ffmpeg','-v','error','-y','-ss',str(start),'-i',str(a.source),
        '-map','0:a:0','-t',str(a.seconds),'-c:a','pcm_f32le',str(wav)],check=True)
    reference = np.frombuffer(subprocess.check_output(['ffmpeg','-v','error','-i',str(wav),
        '-f','f32le','-']),dtype='<f4').reshape(-1,2).astype(np.float64)
    if len(reference) < 16384: p.error('excerpt is too short or beyond end of input')
    for bitrate in a.bitrates:
        for beam in a.beams:
            for masking in a.masking:
                suffix = '-psy' if masking == 'on' else ''
                encoded = root/f'{stem}-{bitrate}-beam{beam}{suffix}.vqf'
                begin = time.perf_counter()
                subprocess.run([exe,'-b',str(bitrate),'--vq-beam',str(beam),
                    '--psychoacoustic' if masking == 'on' else '--no-psychoacoustic',str(wav),str(encoded)],
                               check=True,capture_output=True)
                elapsed = time.perf_counter()-begin
                decoded = subprocess.run(['ffmpeg','-v','error','-i',str(encoded),'-f','f32le','-'],
                                         check=True,capture_output=True)
                pcm = np.frombuffer(decoded.stdout,dtype='<f4').reshape(-1,2).astype(np.float64)
                if len(pcm) < len(reference) or not np.isfinite(pcm).all():
                    raise RuntimeError('invalid decoded PCM')
                # Current FFmpeg has opposite polarity to the encoder's local decoder.
                # Fixed zero lag and unity gain: do not hide alignment/gain errors.
                ref = reference[4096:-4096]
                out = -pcm[4096:len(reference)-4096]
                error = ref-out
                power = np.mean(ref*ref,axis=0)
                mse = np.mean(error*error,axis=0)
                n = len(ref)//1024*1024
                sp = np.sum(ref[:n].reshape(-1,1024,2)**2,axis=(1,2))
                ep = np.sum(error[:n].reshape(-1,1024,2)**2,axis=(1,2))
                active = sp > max(sp.max()*1e-6,1e-15)
                seg = np.mean(np.clip(10*np.log10((sp[active]+1e-30)/(ep[active]+1e-30)),-10,35))
                # Magnitude-only comparison independent of the encoder's masking
                # objective. Fixed 1024-sample Hann windows and a reference-relative
                # -80 dB floor; smaller is better, but this is not a listening score.
                window = np.hanning(1024)[None,:,None]
                ref_mag = np.abs(np.fft.rfft(ref[:n].reshape(-1,1024,2)*window,axis=1))
                out_mag = np.abs(np.fft.rfft(out[:n].reshape(-1,1024,2)*window,axis=1))
                floor = max(float(ref_mag.max())*1e-4,1e-15)
                log_error = 20*np.log10(np.maximum(ref_mag,floor)/np.maximum(out_mag,floor))
                log_spectral_rmse = float(np.sqrt(np.mean(log_error*log_error)))
                row = {'start':start,'kbps':bitrate,'beam':beam,'masking':masking,'encode_seconds':elapsed,
                    'bytes':encoded.stat().st_size,'decoded_frames':len(pcm),
                    'snr_db':(10*np.log10(power/mse)).tolist(),
                    'combined_snr_db':float(10*np.log10(power.sum()/mse.sum())),
                    'segmental_snr_db':float(seg),'log_spectral_rmse_db':log_spectral_rmse,
                    'gain_db':(10*np.log10(np.mean(out*out,axis=0)/power)).tolist(),
                    'peak':float(np.abs(pcm).max()),'over_full_scale_percent':float(100*np.mean(np.abs(pcm)>1)),
                    'demux_warning':bool(decoded.stderr)}
                results['rows'].append(row)
                (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                print(json.dumps(row),flush=True)
