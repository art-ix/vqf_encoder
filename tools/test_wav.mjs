// Usage: node tools/test_wav.mjs path/to/vqf_encode[.exe] [scratch-directory]
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';
const exe = path.resolve(process.argv[2]), dir = path.resolve(process.argv[3] || 'obj/wav-tests');
fs.mkdirSync(dir, {recursive: true});
let expected;
for (const [bits, subtype, extensible] of [[16,1,false], [24,1,false], [32,1,false], [32,3,false], [32,1,true], [32,3,true]]) {
    const fmt = Buffer.alloc(extensible ? 40 : 16);
    fmt.writeUInt16LE(extensible ? 65534 : subtype);
    fmt.writeUInt16LE(2, 2); fmt.writeUInt32LE(44100, 4);
    fmt.writeUInt32LE(44100 * bits / 4, 8); fmt.writeUInt16LE(bits / 4, 12); fmt.writeUInt16LE(bits, 14);
    if (extensible) {
        fmt.writeUInt16LE(22, 16); fmt.writeUInt16LE(bits, 18); fmt.writeUInt32LE(3, 20);
        Buffer.from([subtype,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113]).copy(fmt,24);
    }
    const data = Buffer.alloc(8193 * 2 * bits / 8);
    for (let i = 0; i < 8193 * 2; ++i) {
        const sample = Math.round(6000 * Math.sin(Math.floor(i / 2) * (i % 2 ? 0.07 : 0.11)));
        if (subtype === 3) data.writeFloatLE(sample / 32768, i * 4);
        else data.writeIntLE(sample * 2 ** (bits - 16), i * bits / 8, bits / 8);
    }
    const chunk = (id, payload) => {
        const b = Buffer.alloc(8 + payload.length + (payload.length & 1));
        b.write(id); b.writeUInt32LE(payload.length, 4); payload.copy(b, 8); return b;
    };
    const body = Buffer.concat([Buffer.from('WAVE'), chunk('fmt ',fmt), chunk('JUNK',Buffer.from([7])), chunk('data',data)]);
    const header = Buffer.alloc(8); header.write('RIFF'); header.writeUInt32LE(body.length,4);
    const wav = Buffer.concat([header,body]), name = `${bits}-${subtype}-${extensible}`;
    const input = path.join(dir,name+'.wav'), output = path.join(dir,name+'.vqf');
    fs.writeFileSync(input,wav);
    const run = spawnSync(exe,[input,output],{encoding:'utf8'});
    assert.equal(run.status,0,run.stderr);
    const file = fs.readFileSync(output);
    if (expected) assert.deepEqual(file,expected,'PCM formats encoded differently');
    else expected = file;
    fs.writeFileSync(input,wav.subarray(0,wav.length-1));
    assert.notEqual(spawnSync(exe,[input,output]).status,0,'Truncated WAV accepted');
    if (subtype === 3) {
        wav.writeFloatLE(NaN,wav.length-data.length);
        fs.writeFileSync(input,wav);
        assert.notEqual(spawnSync(exe,[input,output]).status,0,'NaN accepted');
    }
    console.log(name+' passed');
}
console.log('WAV format regression tests passed');
