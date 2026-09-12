// Usage: node tools/compare_audio.mjs reference.wav decoded.wav [decoded-offset-seconds]
// WAV input: PCM 16/24/32 or float32. No external JS packages required.
import fs from 'node:fs';

function readWav(path) {
    const b = fs.readFileSync(path);
    if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE')
        throw new Error('Expected RIFF/WAVE: ' + path);
    let format, channels, rate, bits, data;
    for (let p = 12; p + 8 <= b.length;) {
        const id = b.toString('ascii', p, p + 4), len = b.readUInt32LE(p + 4);
        p += 8;
        if (p + len > b.length) throw new Error('Truncated WAV');
        if (id === 'fmt ') {
            format = b.readUInt16LE(p); channels = b.readUInt16LE(p + 2);
            rate = b.readUInt32LE(p + 4); bits = b.readUInt16LE(p + 14);
            if (format === 65534) format = b.readUInt16LE(p + 24);
        } else if (id === 'data') data = b.subarray(p, p + len);
        p += len + (len & 1);
    }
    if (!data || !((format === 1 && [16, 24, 32].includes(bits)) || (format === 3 && bits === 32)))
        throw new Error('Unsupported WAV encoding');
    const samples = new Float32Array(data.length / (bits / 8));
    for (let i = 0; i < samples.length; ++i)
        samples[i] = format === 3 ? data.readFloatLE(i * 4)
                   : data.readIntLE(i * (bits / 8), bits / 8) / 2 ** (bits - 1);
    return {samples, rate, channels, frames: samples.length / channels};
}

function fft(re, im, inverse = false) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; ++i) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
    }
    for (let len = 2; len <= n; len *= 2) {
        const a = (inverse ? 2 : -2) * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a);
        for (let base = 0; base < n; base += len) {
            let cr = 1, ci = 0;
            for (let j = 0; j < len / 2; ++j) {
                const i = base + j, k = i + len / 2;
                const tr = cr * re[k] - ci * im[k], ti = cr * im[k] + ci * re[k];
                re[k] = re[i] - tr; im[k] = im[i] - ti; re[i] += tr; im[i] += ti;
                const next = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = next;
            }
        }
    }
    if (inverse) for (let i = 0; i < n; ++i) { re[i] /= n; im[i] /= n; }
}

const ref = readWav(process.argv[2]), dec = readWav(process.argv[3]);
if (ref.rate !== dec.rate || ref.channels !== dec.channels) throw new Error('Rate/channel mismatch');
const offset = Math.round(Number(process.argv[4] || 0) * ref.rate);
const n = 131072, start = 8192, count = Math.min(65536, ref.frames - start, dec.frames - offset - start);
if (count < 8192) throw new Error('At least 0.4 seconds of audio required');
const ar = new Float64Array(n), ai = new Float64Array(n), br = new Float64Array(n), bi = new Float64Array(n);
for (let i = 0; i < count; ++i) {
    ar[i] = ref.samples[(start + i) * ref.channels];
    br[i] = dec.samples[(offset + start + i) * dec.channels];
}
fft(ar, ai); fft(br, bi);
for (let i = 0; i < n; ++i) {
    const r = ar[i] * br[i] + ai[i] * bi[i];
    bi[i] = ar[i] * bi[i] - ai[i] * br[i]; br[i] = r;
}
fft(br, bi, true);
let lag = 0, best = -Infinity, polarity = 1;
for (let i = -8192; i <= 8192; ++i) {
    const score = br[(i + n) % n];
    if (Math.abs(score) > best) { best = Math.abs(score); lag = i; polarity = Math.sign(score); }
}
const results = [];
for (let ch = 0; ch < ref.channels; ++ch) {
    let xx = 0, yy = 0, xy = 0, error = 0, peak = 0, clipped = 0, total = 0;
    for (let i = 4096; i < ref.frames - 4096; ++i) {
        const j = i + lag + offset;
        if (j < 0 || j >= dec.frames) continue;
        const x = ref.samples[i * ref.channels + ch], y = polarity * dec.samples[j * dec.channels + ch];
        if (!Number.isFinite(y)) throw new Error('Non-finite decoded PCM');
        xx += x * x; yy += y * y; xy += x * y; error += (x - y) ** 2;
        peak = Math.max(peak, Math.abs(y)); clipped += Math.abs(y) >= 1 ? 1 : 0; ++total;
    }
    results.push({channel: ch, snr_db: 10 * Math.log10(xx / error),
        correlation: xy / Math.sqrt(xx * yy), gain_db: 10 * Math.log10(yy / xx), peak,
        clipped_percent: 100 * clipped / total});
}
console.log(JSON.stringify({reference: process.argv[2], decoded: process.argv[3], lag_samples: lag, polarity, results}, null, 2));
