#pragma once

// End-to-end regression checks, including API chunking and decoder priming.
namespace {
std::vector<float> decode_test_file(const twinvq::Encoder& enc) {
    const auto file = enc.build_file();
    twinvq::VqfInfo info;
    std::string error;
    if (!twinvq::parse_vqf_header_mem(file.data(), file.size(), info, error))
        throw std::runtime_error(error);
    twinvq::Decoder decoder(info);
    twinvq::Packetizer packetizer;
    packetizer.frame_bits = info.frame_bits;
    std::vector<float> result, frame(info.frame_samples * info.channels);
    std::vector<uint8_t> packet;
    size_t offset = static_cast<size_t>(info.data_offset);
    for (int i = 0; i < enc.frames_written(); ++i) {
        const int bytes = packetizer.bytes_to_read();
        if (offset + bytes > file.size()) throw std::runtime_error("truncated encoded frame");
        packet.resize(bytes + 2);
        const int size = packetizer.build(file.data() + offset, packet.data());
        offset += bytes;
        const int count = decoder.decode_packet(packet.data(), size, frame.data());
        result.insert(result.end(), frame.begin(), frame.begin() + count * info.channels);
    }
    for (float x : result)
        if (!std::isfinite(x)) throw std::runtime_error("non-finite decoded PCM");
    return result;
}

// Read each window before locating history flags; subblocks add gain fields
// and omit PPC. Encoder enforces exact, unpadded frame bit counts.
int bark_history_flags(const twinvq::Encoder& enc) {
    const auto& mode = *enc.mode();
    auto bit = [&](size_t pos) { return (enc.data().at(pos / 8) >> (7 - pos % 8)) & 1; };
    int total = 0;
    for (int frame = 0; frame < enc.frames_written(); ++frame) {
        const size_t start = static_cast<size_t>(frame) * enc.frame_bits();
        int window = 0;
        for (int i = 0; i < twinvq::kWindowTypeBits; ++i) window = window * 2 + bit(start + i);
        if (window > 8) throw std::runtime_error("invalid encoded window");
        const int types[] = {2, 2, 0, 2, 1, 2, 2, 1, 1};
        const int type = types[window], sub = mode.fmode[type].sub;
        const int following = enc.channels() * (twinvq::kGainBits + mode.lsp_bit0 + mode.lsp_bit1 +
            mode.lsp_split * mode.lsp_bit2 + (type == 2 ? mode.ppc_period_bit + mode.ppc_shape_bit + mode.pgain_bit
                                                       : sub * twinvq::kSubGainBits));
        const int offset = enc.frame_bits() - following - enc.channels() * sub;
        for (int j = 0; j < enc.channels() * sub; ++j) total += bit(start + offset + j);
    }
    return total;
}

int test_codec(bool lsp_search = twinvq::Encoder::Config{}.lsp_search,
               bool bark_search = twinvq::Encoder::Config{}.bark_search,
               bool psychoacoustic = false,
               twinvq::Encoder::BlockMode blocks = twinvq::Encoder::BlockMode::Long) {
    int history_flags = 0;
    int mode_count = 0;
    const auto* modes = twinvq::legal_modes(mode_count);
    for (int m = 0; m < mode_count; ++m) for (int channels = 1; channels <= 2; ++channels) {
        twinvq::Encoder::Config cfg;
        cfg.sample_rate = modes[m].sample_rate;
        cfg.bitrate_kbps = modes[m].kbps_per_channel * channels;
        cfg.channels = channels;
        cfg.lsp_search = lsp_search;
        cfg.bark_search = bark_search;
        cfg.psychoacoustic = psychoacoustic;
        cfg.block_mode = blocks;
        twinvq::Encoder whole(cfg), chunked(cfg);
        const int hop = whole.frame_samples(), frames = 7 * hop + 17;
        std::vector<float> pcm(frames * channels);
        for (int i = 0; i < frames; ++i) for (int ch = 0; ch < channels; ++ch) {
            const double t = static_cast<double>(i) / cfg.sample_rate;
            pcm[i * channels + ch] = static_cast<float>(
                0.18 * std::sin(6.283185307179586 * ((ch ? 653 : 437) * t + 70 * t * t))
              + 0.06 * std::sin(6.283185307179586 * (ch ? 1379 : 1123) * t));
        }
        whole.feed(pcm.data(), frames);
        whole.flush();
        history_flags += bark_history_flags(whole);
        for (int f = 0; f < whole.frames_written(); ++f) {
            int window = 0;
            for (int k = 0; k < twinvq::kWindowTypeBits; ++k) {
                const size_t b = static_cast<size_t>(f) * whole.frame_bits() + k;
                window = window * 2 + ((whole.data().at(b / 8) >> (7 - b % 8)) & 1);
            }
            const bool short_mode = blocks == twinvq::Encoder::BlockMode::Short;
            const int expected = blocks == twinvq::Encoder::BlockMode::Long || f == 0 ? 0
                               : f == whole.frames_written() - 1 ? (short_mode ? 3 : 5) : (short_mode ? 2 : 8);
            if (window != expected) throw std::runtime_error("incorrect fixed-block window schedule");
        }
        int pos = 0;
        const int chunks[] = {1, 3, hop - 1, hop + 5};
        for (int k = 0; pos < frames; ++k) {
            const int count = std::min(chunks[k % 4], frames - pos);
            chunked.feed(nullptr, 0);
            chunked.feed(pcm.data() + pos * channels, count);
            pos += count;
        }
        chunked.flush();
        chunked.flush();
        if (whole.data() != chunked.data()) throw std::runtime_error("feed chunking changed the bitstream");
        if (whole.frames_written() != (frames + hop - 1) / hop + 2)
            throw std::runtime_error("incorrect priming/tail frame count");
        auto out = decode_test_file(whole);
        if (out.size() != static_cast<size_t>((frames + hop - 1) / hop * hop * channels))
            throw std::runtime_error("lost end of input");
        double signal = 0, error = 0, output = 0;
        for (int i = hop * channels; i < (frames - hop) * channels; ++i) {
            signal += static_cast<double>(pcm[i]) * pcm[i];
            error += static_cast<double>(pcm[i] - out[i]) * (pcm[i] - out[i]);
            output += static_cast<double>(out[i]) * out[i];
        }
        const double snr = 10 * std::log10(signal / error);
        const double gain = 10 * std::log10(output / signal);
        std::cout << cfg.sample_rate << " Hz " << cfg.bitrate_kbps << " kbps " << channels
                  << " ch: SNR=" << snr << " dB gain=" << gain << " dB\n";
        if (snr < 10 || std::fabs(gain) > 3)
            throw std::runtime_error("roundtrip quality/gain regression");
        // This deterministic chirp previously scored about 27 dB with angular
        // LSP search alone. Protect the spectral candidate's measured gain.
        if (blocks == twinvq::Encoder::BlockMode::Long && lsp_search && cfg.sample_rate == 16000 && channels == 1 && snr < 30)
            throw std::runtime_error("spectral LSP quality regression");
        twinvq::Encoder silent(cfg);
        std::fill(pcm.begin(), pcm.end(), 0.0f);
        silent.feed(pcm.data(), frames); silent.flush();
        out = decode_test_file(silent);
        float peak = 0;
        for (float x : out) peak = std::max(peak, std::fabs(x));
        std::cout << "  silence peak=" << peak << "\n";
        if (peak > 0.001f) throw std::runtime_error("excessive noise on silent input");
    }
    if (bark_search ? history_flags == 0 : history_flags != 0)
        throw std::runtime_error("Bark history flags do not exercise the requested mode");
    std::cout << "transmitted Bark history flags=" << history_flags << "\n";
    std::cout << "codec regression tests passed\n";
    return 0;
}
} // namespace
