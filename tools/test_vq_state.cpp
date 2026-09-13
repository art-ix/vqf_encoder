// Compare two VQF files: final main-VQ polishing must preserve all other fields.
#include "twinvq/twinvq_encoder.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: test_vq_state BEFORE.vqf AFTER.vqf");
        std::vector<uint8_t> data[2];
        twinvq::VqfInfo info[2];
        for (int i = 0; i < 2; ++i) {
            std::ifstream file(argv[i + 1], std::ios::binary);
            if (!file) throw std::runtime_error("cannot open input");
            data[i].assign(std::istreambuf_iterator<char>(file), {});
            std::string error;
            if (!twinvq::parse_vqf_header_mem(data[i].data(), data[i].size(), info[i], error))
                throw std::runtime_error(error);
        }
        const auto& a = info[0];
        const auto& b = info[1];
        if (a.sample_rate != b.sample_rate || a.channels != b.channels ||
            a.frame_bits != b.frame_bits || a.bitrate_kbps != b.bitrate_kbps ||
            a.data_size != b.data_size || data[0].size() != data[1].size())
            throw std::runtime_error("mode or bit budget changed");
        twinvq::Encoder::Config cfg;
        cfg.sample_rate = a.sample_rate; cfg.channels = a.channels;
        cfg.bitrate_kbps = a.bitrate_kbps; cfg.threads = 1;
        twinvq::Encoder encoder(cfg);
        const auto& mode = *encoder.mode();
        auto bit = [&](int file, size_t pos) {
            return (data[file].at(info[file].data_offset + pos / 8) >> (7 - pos % 8)) & 1;
        };
        const size_t frames = a.data_size * 8 / a.frame_bits;
        size_t changed = 0;
        const int types[] = {2, 2, 0, 2, 1, 2, 2, 1, 1};
        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t start = frame * a.frame_bits;
            int window = 0;
            for (int j = 0; j < twinvq::kWindowTypeBits; ++j) {
                if (bit(0, start + j) != bit(1, start + j))
                    throw std::runtime_error("window schedule changed");
                window = 2 * window + bit(0, start + j);
            }
            if (window > 8) throw std::runtime_error("invalid window");
            const int type = types[window];
            const auto& fm = mode.fmode[type];
            const int trailer = a.channels * (fm.sub * (fm.bark_n_coef * fm.bark_n_bit + 1)
                + twinvq::kGainBits + mode.lsp_bit0 + mode.lsp_bit1 + mode.lsp_split * mode.lsp_bit2
                + (type == 2 ? mode.ppc_shape_bit + mode.ppc_period_bit + mode.pgain_bit
                             : fm.sub * twinvq::kSubGainBits));
            const size_t end_main = a.frame_bits - trailer;
            for (size_t j = twinvq::kWindowTypeBits; j < static_cast<size_t>(a.frame_bits); ++j) {
                if (bit(0, start + j) == bit(1, start + j)) continue;
                if (j >= end_main) throw std::runtime_error("envelope, gain, history or PPC changed");
                ++changed;
            }
        }
        std::cout << frames << " frames: non-main fields unchanged; " << changed << " main-VQ bits changed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
