#include "twinvq/twinvq_encoder.hpp"
#include "twinvq/twinvq_decoder.hpp"
#include "twinvq/vqf_file.hpp"
#include "twinvq_mdct.hpp"
#include "twinvq_window.hpp"
#include "twinvq_psychoacoustic.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec_tests.hpp"
#include "resample.hpp"
#include "resample_tests.hpp"

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

struct Wav {
    int rate = 0;
    int channels = 0;
    std::vector<float> pcm;
};

Wav read_wav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 44 || std::memcmp(file.data(), "RIFF", 4) != 0 ||
        std::memcmp(file.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAV file");
    size_t pos = 12;
    int rate = 0, ch = 0, bps = 0, format = 0, block_align = 0;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    while (pos + 8 <= file.size()) {
        char id[5] = {};
        std::memcpy(id, file.data() + pos, 4);
        const uint32_t sz = rd32(file.data() + pos + 4);
        pos += 8;
        if (sz > file.size() - pos)
            throw std::runtime_error("truncated WAV chunk");
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            format = rd16(file.data() + pos);
            ch = rd16(file.data() + pos + 2);
            rate = static_cast<int>(rd32(file.data() + pos + 4));
            block_align = rd16(file.data() + pos + 12);
            bps = rd16(file.data() + pos + 14);
            if (format == 0xFFFE) {
                static const uint8_t guid_tail[] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
                if (sz < 40 || rd16(file.data() + pos + 16) < 22 ||
                    std::memcmp(file.data() + pos + 26, guid_tail, sizeof(guid_tail)) != 0)
                    throw std::runtime_error("unsupported extensible WAV format");
                format = rd16(file.data() + pos + 24);
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = file.data() + pos;
            data_size = sz;
        }
        pos += sz + (sz & 1);
    }
    if (!data || rate <= 0 || ch < 1 || ch > 2 ||
        !((format == 1 && (bps == 16 || bps == 24 || bps == 32)) || (format == 3 && bps == 32)) ||
        block_align != (bps / 8) * ch || data_size % block_align != 0)
        throw std::runtime_error("unsupported WAV (need 16/24/32-bit PCM or float32, 1-2 ch)");
    Wav w;
    w.rate = rate;
    w.channels = ch;
    const int stride = (bps / 8) * ch;
    const size_t frames = data_size / static_cast<size_t>(stride);
    w.pcm.resize(frames * static_cast<size_t>(ch));
    for (size_t i = 0; i < frames; i++) {
        const uint8_t* s = data + i * static_cast<size_t>(stride);
        for (int c = 0; c < ch; c++) {
            float v = 0;
            if (format == 3) {
                const uint32_t u = rd32(s + c * 4);
                std::memcpy(&v, &u, sizeof(v));
                if (!std::isfinite(v)) throw std::runtime_error("non-finite float WAV sample");
            } else if (bps == 16) {
                v = static_cast<float>(static_cast<int16_t>(rd16(s + c * 2))) / 32768.0f;
            } else if (bps == 24) {
                int32_t x = (s[c * 3] | (s[c * 3 + 1] << 8) | (s[c * 3 + 2] << 16));
                if (x & 0x800000)
                    x |= ~0xFFFFFF;
                v = static_cast<float>(x) / 8388608.0f;
            } else {
                const uint32_t u = rd32(s + c * 4);
                int32_t x;
                std::memcpy(&x, &u, 4);
                v = static_cast<float>(x) / 2147483648.0f;
            }
            w.pcm[i * static_cast<size_t>(ch) + static_cast<size_t>(c)] = v;
        }
    }
    return w;
}

void usage() {
    std::cerr << "usage: vqf_encode [options] input.wav output.vqf\n"
              << "       vqf_encode --list-modes\n"
              << "       vqf_encode --test-mdct\n"
              << "       vqf_encode --test-codec\n"
              << "       vqf_encode --test-codec-psychoacoustic\n"
              << "       vqf_encode --test-codec-basic\n"
              << "       vqf_encode --test-codec-lsp\n"
              << "       vqf_encode --test-codec-bark\n"
              << "       vqf_encode --test-codec-search\n"
              << "       vqf_encode --test-resample\n"
              << "       vqf_encode --test-roundtrip [seconds]\n"
              << "\noptions:\n"
              << "  -b, --bitrate KBPS   total bitrate; snaps to a legal TwinVQ mode\n"
              << "                       (44.1 kHz stereo max is 96 = 48 kbps/ch; there is no 128)\n"
              << "  --title/--artist/--album/--year/--track/--genre/--comment TEXT\n"
              << "  --bark-search       enable Bark/history search (default)\n"
              << "  --lsp-search        enable frame-scored LSP search (default)\n"
              << "  --no-bark-search    disable Bark/history search\n"
              << "  --no-lsp-search     disable LSP search\n"
              << "  --psychoacoustic    experimental masking weights (default: off)\n"
              << "  --no-psychoacoustic disable masking weights\n"
              << "  --block-mode MODE  experimental fixed blocks: long (default), short, medium\n"
              << "  --vq-beam N        VQ breadth: auto (default), 4, 8, 16, 32\n"
              << "  --no-delay           do not prepend priming frames\n"
              << "\nfoobar2000 Converter:\n"
              << "  Encoder     vqf_encode.exe\n"
              << "  Extension   vqf\n"
              << "  Parameters  -b 96 %s %d\n";
}

void list_modes() {
    int n = 0;
    const auto* modes = twinvq::legal_modes(n);
    std::cout << "rate_hz  kbps/ch  stereo_total  frame\n";
    for (int i = 0; i < n; i++) {
        std::cout << "  " << modes[i].sample_rate << "    " << modes[i].kbps_per_channel << "       "
                  << (modes[i].kbps_per_channel * 2) << "            " << modes[i].frame_samples << "\n";
    }
    std::cout << "\nVQF (NTT TwinVQ / Yamaha SoundVQ) has no 56/64 kbps-per-channel codebook,\n"
                 "so 112/128/160/192 kbps stereo at 44.1 kHz cannot be encoded. Max is 96 kbps.\n";
}


int test_roundtrip(double seconds) {
    twinvq::Encoder::Config cfg;
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    cfg.bitrate_kbps = 96;
    cfg.compensate_delay = true;
    twinvq::Encoder enc(cfg);
    const int rate = enc.sample_rate();
    const int ch = enc.channels();
    const int n = static_cast<int>(seconds * rate);
    std::vector<float> pcm(static_cast<size_t>(n) * ch);
    for (int i = 0; i < n; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const float s = 0.35f * std::sin(2.0f * 3.14159265f * 440.0f * t);
        pcm[static_cast<size_t>(i) * ch] = s;
        if (ch == 2)
            pcm[static_cast<size_t>(i) * ch + 1] = 0.25f * std::sin(2.0f * 3.14159265f * 660.0f * t);
    }
    enc.feed(pcm.data(), n);
    enc.flush();
    const auto file = enc.build_file();
    twinvq::VqfInfo info;
    std::string err;
    if (!twinvq::parse_vqf_header_mem(file.data(), file.size(), info, err)) {
        std::cerr << "encoded header parse failed: " << err << "\n";
        return 1;
    }
    twinvq::Decoder dec(info);
    twinvq::Packetizer pkt;
    pkt.frame_bits = info.frame_bits;
    const uint8_t* data = file.data() + info.data_offset;
    const size_t data_size = file.size() - info.data_offset;
    size_t off = 0;
    std::vector<float> out;
    std::vector<float> frame(static_cast<size_t>(info.channels) * info.frame_samples);
    std::vector<uint8_t> packet, file_bytes;
    while (off < data_size) {
        const int nread = pkt.bytes_to_read();
        if (off + static_cast<size_t>(nread) > data_size)
            break;
        file_bytes.assign(data + off, data + off + nread);
        off += static_cast<size_t>(nread);
        packet.resize(static_cast<size_t>(nread) + 2);
        const int psz = pkt.build(file_bytes.data(), packet.data());
        const int got = dec.decode_packet(packet.data(), psz, frame.data());
        if (got > 0)
            out.insert(out.end(), frame.begin(), frame.begin() + got * info.channels);
    }
    if (out.size() < 2048) {
        std::cerr << "roundtrip produced too few samples: " << out.size() << "\n";
        return 1;
    }
    const size_t skip = static_cast<size_t>(info.frame_samples) * 2 * info.channels;
    double num = 0, den = 0, peak = 0;
    const size_t lim = std::min(out.size(), pcm.size());
    int best_lag = 0;
    double best_corr = -1e99;
    const int chn = info.channels;
    const int max_lag = info.frame_samples * 2;
    for (int lag = -max_lag; lag <= max_lag; lag += 64) {
        double corr = 0, e1 = 0, e2 = 0;
        int count = 0;
        for (size_t i = skip; i + 4 < lim; i += 4) {
            const long si = static_cast<long>(i) + static_cast<long>(lag) * chn;
            if (si < 0 || static_cast<size_t>(si) >= out.size())
                continue;
            corr += static_cast<double>(pcm[i]) * out[static_cast<size_t>(si)];
            e1 += static_cast<double>(pcm[i]) * pcm[i];
            e2 += static_cast<double>(out[static_cast<size_t>(si)]) * out[static_cast<size_t>(si)];
            count++;
        }
        if (count > 100 && e1 > 0 && e2 > 0) {
            const double ncorr = corr / std::sqrt(e1 * e2);
            if (ncorr > best_corr) {
                best_corr = ncorr;
                best_lag = lag;
            }
        }
    }
    for (size_t i = skip; i < lim; i++) {
        const long si = static_cast<long>(i) + static_cast<long>(best_lag) * chn;
        const double o = (si >= 0 && static_cast<size_t>(si) < out.size()) ? out[static_cast<size_t>(si)] : 0;
        const double e = static_cast<double>(pcm[i]) - o;
        num += e * e;
        den += static_cast<double>(pcm[i]) * pcm[i];
        peak = std::max(peak, std::fabs(static_cast<double>(out[i])));
    }
    const double snr = (num > 0 && den > 0) ? 10.0 * std::log10(den / num) : 99.0;
    std::cout << "mode        " << info.sample_rate << " Hz / " << info.bitrate_kbps << " kbps / "
              << info.channels << " ch\n";
    std::cout << "frames      " << enc.frames_written() << "  data " << enc.data_size() << " bytes\n";
    std::cout << "decoded     " << out.size() / info.channels << " samples, peak=" << peak << "\n";
    std::cout << "align lag   " << best_lag << " samples, corr=" << best_corr << "\n";
    std::cout << "snr         " << snr << " dB\n";
    if (peak < 1.0e-4) {
        std::cerr << "roundtrip produced silence\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    if (argc >= 2 && std::string(argv[1]) == "--test-resample")
        return test_resample();
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-bark")
        return test_codec(false, true);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-search")
        return test_codec(true, true);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-basic")
        return test_codec(false, false);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-lsp")
        return test_codec(true, false);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-psychoacoustic")
        return test_codec(true, true, true);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-short")
        return test_codec(true, true, true, twinvq::Encoder::BlockMode::Short);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec-medium")
        return test_codec(true, true, true, twinvq::Encoder::BlockMode::Medium);
    if (argc >= 2 && std::string(argv[1]) == "--test-codec")
        return test_codec();
    if (argc >= 2 && std::string(argv[1]) == "--list-modes") {
        list_modes();
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--test-mdct") {
        float e1 = 0, e2 = 0, e3 = 0, e4 = 0, e5 = 0;
        const bool a = twinvq::imdct_self_test(&e1);
        const bool b = twinvq::mdct_roundtrip_test(&e2);
        const bool c = twinvq::mdct_self_test(&e3);
        const bool d = twinvq::lpc_analysis_self_test(&e4);
        std::cout << "imdct self-test " << (a ? "ok" : "FAIL") << " max abs err=" << e1 << "\n";
        std::cout << "mdct roundtrip  " << (b ? "ok" : "FAIL") << " max abs err=" << e2 << "\n";
        std::cout << "mdct self-test  " << (c ? "ok" : "FAIL") << " max abs err=" << e3 << "\n";
        std::cout << "lpc self-test   " << (d ? "ok" : "FAIL") << " max abs err=" << e4 << "\n";
        const bool e = twinvq::window_transition_self_test(&e5);
        std::cout << "window transitions " << (e ? "ok" : "FAIL") << " max abs err=" << e5 << "\n";
        const bool p = twinvq::psychoacoustic_self_test();
        std::cout << "psychoacoustic model " << (p ? "ok" : "FAIL") << "\n";
        return (a && b && c && d && e && p) ? 0 : 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "--test-roundtrip") {
        const double sec = (argc >= 3) ? std::atof(argv[2]) : 0.6;
        return test_roundtrip(sec);
    }

    twinvq::Encoder::Config cfg;
    cfg.bitrate_kbps = 0;
    std::string in_path, out_path;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--list-modes") {
            list_modes();
            return 0;
        } else if (a == "-b" || a == "--bitrate") {
            cfg.bitrate_kbps = std::stoi(need("-b"));
        } else if (a == "--title") {
            cfg.tags.title = need("--title");
        } else if (a == "--artist") {
            cfg.tags.artist = need("--artist");
        } else if (a == "--album") {
            cfg.tags.album = need("--album");
        } else if (a == "--year") {
            cfg.tags.year = need("--year");
        } else if (a == "--track") {
            cfg.tags.track = need("--track");
        } else if (a == "--genre") {
            cfg.tags.genre = need("--genre");
        } else if (a == "--comment") {
            cfg.tags.comment = need("--comment");
        } else if (a == "--no-bark-search") {
            cfg.bark_search = false;
        } else if (a == "--no-lsp-search") {
            cfg.lsp_search = false;
        } else if (a == "--psychoacoustic") {
            cfg.psychoacoustic = true;
        } else if (a == "--no-psychoacoustic") {
            cfg.psychoacoustic = false;
        } else if (a == "--block-mode") {
            const std::string value = need("--block-mode");
            if (value == "long") cfg.block_mode = twinvq::Encoder::BlockMode::Long;
            else if (value == "short") cfg.block_mode = twinvq::Encoder::BlockMode::Short;
            else if (value == "medium") cfg.block_mode = twinvq::Encoder::BlockMode::Medium;
            else throw std::invalid_argument("block mode must be long, short or medium");
        } else if (a == "--vq-beam") {
            const std::string value = need("--vq-beam");
            if (value == "auto") cfg.vq_beam = 0;
            else if (value == "4") cfg.vq_beam = 4;
            else if (value == "8") cfg.vq_beam = 8;
            else if (value == "16") cfg.vq_beam = 16;
            else if (value == "32") cfg.vq_beam = 32;
            else throw std::invalid_argument("VQ beam must be auto, 4, 8, 16 or 32");
        } else if (a == "--bark-search") {
            cfg.bark_search = true;
        } else if (a == "--lsp-search") {
            cfg.lsp_search = true;
        } else if (a == "--no-delay") {
            cfg.compensate_delay = false;
        } else if (a[0] == '-') {
            throw std::runtime_error("unknown option " + a);
        } else if (in_path.empty()) {
            in_path = a;
        } else if (out_path.empty()) {
            out_path = a;
        } else {
            throw std::runtime_error("extra argument " + a);
        }
    }
    if (in_path.empty() || out_path.empty()) {
        usage();
        return 1;
    }

    const Wav wav = read_wav(in_path);
    int out_rate = 0, out_br = 0;
    std::string err;
    const int want = cfg.bitrate_kbps;
    if (!twinvq::pick_encoder_mode(wav.rate, wav.channels, cfg.bitrate_kbps, out_rate, out_br, err))
        throw std::runtime_error(err);
    if (want > 0 && want != out_br) {
        std::cerr << "note: " << want << " kbps is not a TwinVQ/VQF mode; using " << out_br
                  << " kbps (" << (out_br / wav.channels) << " kbps/ch at " << out_rate << " Hz)\n";
        if (want >= 112 && out_rate == 44100)
            std::cerr << "      44.1 kHz stereo max is 96 kbps (48 kbps/ch); no 128k codebook exists.\n";
    }
    cfg.sample_rate = out_rate;
    cfg.channels = wav.channels;
    cfg.bitrate_kbps = out_br;

    std::vector<float> pcm;
    if (wav.rate != out_rate) {
        pcm = audio::resample(wav.pcm, wav.channels, wav.rate, out_rate);
        std::cerr << "resampled " << wav.rate << " -> " << out_rate << " Hz\n";
    } else {
        pcm = wav.pcm;
    }

    twinvq::Encoder enc(cfg);
    const int frames = static_cast<int>(pcm.size() / static_cast<size_t>(enc.channels()));
    enc.feed(pcm.data(), frames);
    enc.flush();
    const auto file = enc.build_file();

    std::ofstream out(out_path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + out_path);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));

    std::cout << "mode        " << enc.sample_rate() << " Hz / " << enc.bitrate_kbps() << " kbps / "
              << enc.channels() << " ch\n";
    std::cout << "frames      " << enc.frames_written() << "\n";
    std::cout << "wrote       " << out_path << " (" << file.size() << " bytes)\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
}
