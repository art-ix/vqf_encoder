#include "twinvq/vqf_file.hpp"
#include "twinvq/twinvq_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Wav16Writer {
public:
    Wav16Writer(const std::string& path, int rate, int channels)
        : out_(path, std::ios::binary) {
        if (!out_)
            throw std::runtime_error("cannot open output " + path);
        out_.exceptions(std::ios::failbit | std::ios::badbit);
        out_.write("RIFF", 4);
        wr32(36);
        out_.write("WAVEfmt ", 8);
        wr32(16);
        wr16(1);
        wr16(static_cast<uint16_t>(channels));
        wr32(static_cast<uint32_t>(rate));
        wr32(static_cast<uint32_t>(rate * channels * 2));
        wr16(static_cast<uint16_t>(channels * 2));
        wr16(16);
        out_.write("data", 4);
        wr32(0);
    }

    void write(const float* samples, size_t count) {
        constexpr uint32_t max_data_bytes = std::numeric_limits<uint32_t>::max() - 36;
        if (count > (max_data_bytes - data_bytes_) / 2)
            throw std::runtime_error("output exceeds the RIFF/WAV size limit");
        buffer_.resize(count * 2);
        for (size_t i = 0; i < count; i++) {
            const float x = std::clamp(samples[i], -1.0f, 1.0f);
            const auto pcm = static_cast<uint16_t>(static_cast<int16_t>(std::lrint(x * 32767.0f)));
            buffer_[i * 2] = static_cast<char>(pcm);
            buffer_[i * 2 + 1] = static_cast<char>(pcm >> 8);
        }
        out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        data_bytes_ += static_cast<uint32_t>(buffer_.size());
    }

    void finish() {
        out_.seekp(4);
        wr32(36 + data_bytes_);
        out_.seekp(40);
        wr32(data_bytes_);
        out_.close();
    }

private:
    void wr32(uint32_t v) {
        const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
        out_.write(b, 4);
    }

    void wr16(uint16_t v) {
        const char b[2] = {char(v), char(v >> 8)};
        out_.write(b, 2);
    }

    std::ofstream out_;
    std::vector<char> buffer_;
    uint32_t data_bytes_ = 0;
};

} // namespace

static int test_tags(const std::string& path) {
    auto matches_tags = [](const twinvq::VqfInfo& info, const twinvq::VqfTags& tags) {
        return info.title == tags.title && info.artist == tags.artist && info.comment == tags.comment &&
               info.copyright == tags.copyright && info.album == tags.album && info.genre == tags.genre &&
               info.track == tags.track && info.year == tags.year && info.composer == tags.composer &&
               info.publisher == tags.publisher;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    twinvq::VqfInfo info;
    std::string err;
    if (!twinvq::parse_vqf_header_mem(file.data(), file.size(), info, err)) {
        std::cerr << "parse: " << err << "\n";
        return 1;
    }
    const auto orig = twinvq::serialize_vqf_header(info);
    if (orig.size() != info.data_offset) {
        std::cerr << "serialize size " << orig.size() << " != data_offset " << info.data_offset << "\n";
        return 1;
    }
    twinvq::VqfInfo again;
    if (!twinvq::parse_vqf_header_mem(orig.data(), orig.size(), again, err)) {
        std::cerr << "reparse: " << err << "\n";
        return 1;
    }
    if (again.title != info.title || again.artist != info.artist || again.channels != info.channels) {
        std::cerr << "roundtrip metadata mismatch\n";
        return 1;
    }

    twinvq::VqfTags tags;
    tags.title = "Tag Write Test — La Grange";
    tags.artist = "Test artist";
    tags.comment = "Test comment";
    tags.copyright = "Test copyright";
    tags.album = "Tres Hombres";
    tags.genre = "Rock";
    tags.track = "3";
    tags.year = "1973";
    tags.composer = "Test composer";
    tags.publisher = "Test publisher";
    twinvq::apply_tags(info, tags, false);
    const auto hdr = twinvq::serialize_vqf_header(info);
    twinvq::VqfInfo tagged;
    if (!twinvq::parse_vqf_header_mem(hdr.data(), hdr.size(), tagged, err)) {
        std::cerr << "tagged parse: " << err << "\n";
        return 1;
    }
    if (!matches_tags(tagged, tags) || hdr.size() != info.data_offset) {
        std::cerr << "apply_tags mismatch: title=[" << tagged.title << "] artist=[" << tagged.artist
                  << "] album=[" << tagged.album << "] year=[" << tagged.year << "] track=[" << tagged.track
                  << "]\n";
        return 1;
    }

    // Binary YEAR and both track IDs must keep their legacy interpretation.
    const twinvq::VqfChunk unknown{{'T', 'E', 'S', 'T'}, {0, 0x80, 0xFF}};
    tagged.chunks.push_back(unknown);
    twinvq::VqfInfo binary;
    for (const char* track_id : {"TRCK", "TRAC"}) {
        for (auto& chunk : tagged.chunks) {
            if (std::memcmp(chunk.id, "TRCK", 4) == 0) {
                std::memcpy(chunk.id, track_id, 4);
                chunk.payload = {0, 7};
            } else if (std::memcmp(chunk.id, "YEAR", 4) == 0) {
                chunk.payload = {0x07, 0xB5}; // 1973
            }
        }
        const auto binary_header = twinvq::serialize_vqf_header(tagged);
        if (!twinvq::parse_vqf_header_mem(binary_header.data(), binary_header.size(), binary, err) ||
            binary.track != "7" || binary.year != "1973") {
            std::cerr << "binary tag parse mismatch: " << err << "\n";
            return 1;
        }
    }

    // Replacing/removing tags must retain audio properties and unknown chunks.
    for (bool strip_all : {false, true}) {
        twinvq::apply_tags(binary, tags, strip_all);
        const auto updated_header = twinvq::serialize_vqf_header(binary);
        twinvq::VqfInfo updated;
        if (!twinvq::parse_vqf_header_mem(updated_header.data(), updated_header.size(), updated, err)) {
            std::cerr << "updated tag parse: " << err << "\n";
            return 1;
        }
        const bool have_unknown = std::any_of(updated.chunks.begin(), updated.chunks.end(), [&](const auto& c) {
            return std::memcmp(c.id, unknown.id, 4) == 0 && c.payload == unknown.payload;
        });
        if (!matches_tags(updated, strip_all ? twinvq::VqfTags{} : tags) || !have_unknown ||
            updated_header.size() != binary.data_offset || updated.data_size != info.data_size ||
            updated.channels != info.channels || updated.sample_rate != info.sample_rate ||
            updated.bitrate_kbps != info.bitrate_kbps) {
            std::cerr << "tag replacement/removal mismatch\n";
            return 1;
        }
    }
    std::cout << "tag roundtrip, binary tags and removal ok, header " << orig.size() << " -> " << hdr.size() << " bytes\n";
    return 0;
}

static int test_seek_start(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
    }
    twinvq::VqfInfo info;
    std::string err;
    auto read = [&](void* dst, size_t n) -> size_t {
        in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(in.gcount());
    };
    if (!twinvq::parse_vqf_header(read, info, err)) {
        std::cerr << "parse: " << err << "\n";
        return 1;
    }

    auto decode_n = [&](twinvq::Packetizer pkt, bool seek_to_zero, int want_frames, std::vector<float>& out) -> int {
        in.clear();
        const int64_t bit_pos = 0;
        if (seek_to_zero)
            pkt.seek_prep(bit_pos);
        in.seekg(twinvq::Packetizer::file_offset_for_bit(bit_pos, info.data_offset), std::ios::beg);
        twinvq::Decoder dec(info);
        std::vector<float> frame(static_cast<size_t>(info.channels) * info.frame_samples);
        std::vector<uint8_t> file_bytes;
        std::vector<uint8_t> packet;
        int got_frames = 0;
        while (got_frames < want_frames) {
            const int nread = pkt.bytes_to_read();
            file_bytes.resize(static_cast<size_t>(nread));
            in.read(reinterpret_cast<char*>(file_bytes.data()), nread);
            if (in.gcount() < nread)
                break;
            packet.resize(static_cast<size_t>(nread) + 2);
            const int psz = pkt.build(file_bytes.data(), packet.data());
            const int got = dec.decode_packet(packet.data(), psz, frame.data());
            if (got > 0) {
                out.insert(out.end(), frame.begin(), frame.begin() + got * info.channels);
                got_frames += got;
            }
        }
        return got_frames;
    };

    twinvq::Packetizer fresh;
    fresh.frame_bits = info.frame_bits;
    std::vector<float> a, b;
    const int n = decode_n(fresh, false, info.frame_samples * 4, a);
    twinvq::Packetizer seeked;
    seeked.frame_bits = info.frame_bits;
    const int m = decode_n(seeked, true, info.frame_samples * 4, b);
    if (n <= 0 || n != m || a.size() != b.size()) {
        std::cerr << "seek-start length mismatch: " << n << " vs " << m << "\n";
        return 1;
    }
    float worst = 0;
    for (size_t i = 0; i < a.size(); i++)
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    if (worst > 1.0e-6f) {
        std::cerr << "seek-start pcm mismatch, max abs err=" << worst << "\n";
        return 1;
    }
    std::cout << "seek-start ok, " << n << " samples compared, max abs err=" << worst << "\n";
    return 0;
}

int main(int argc, char** argv) try {
    if (argc >= 2 && std::string(argv[1]) == "--test-imdct") {
        float err = 0;
        if (!twinvq::imdct_self_test(&err)) {
            std::cerr << "imdct self-test failed, max abs err=" << err << "\n";
            return 1;
        }
        std::cout << "imdct self-test ok, max abs err=" << err << "\n";
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--test-tags") {
        if (argc < 3) {
            std::cerr << "usage: vqf_decode --test-tags <input.vqf>\n";
            return 1;
        }
        return test_tags(argv[2]);
    }
    if (argc >= 2 && std::string(argv[1]) == "--test-seek") {
        if (argc < 3) {
            std::cerr << "usage: vqf_decode --test-seek <input.vqf>\n";
            return 1;
        }
        return test_seek_start(argv[2]);
    }
    if (argc < 2) {
        std::cerr << "usage: vqf_decode [--test-imdct] [--test-tags] [--test-seek] <input.vqf> [output.wav]\n";
        return 1;
    }
    const std::string in_path = argv[1];
    const std::string out_path = (argc >= 3) ? argv[2] : (in_path + ".wav");

    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << in_path << "\n";
        return 1;
    }

    twinvq::VqfInfo info;
    std::string err;
    auto read = [&](void* dst, size_t n) -> size_t {
        in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(in.gcount());
    };
    if (!twinvq::parse_vqf_header(read, info, err)) {
        std::cerr << "parse error: " << err << "\n";
        return 1;
    }

    std::cout << "version     " << info.version << "\n";
    std::cout << "title       " << info.title << "\n";
    std::cout << "artist      " << info.artist << "\n";
    std::cout << "channels    " << info.channels << "\n";
    std::cout << "sample_rate " << info.sample_rate << "\n";
    std::cout << "bitrate     " << info.bitrate_kbps << " kbps\n";
    std::cout << "frame       " << info.frame_samples << " samples, " << info.frame_bits << " bits\n";
    std::cout << "data        offset=" << info.data_offset << " size=" << info.data_size << "\n";
    std::cout << "duration    " << twinvq::duration_seconds(info) << " s\n";

    twinvq::Decoder dec(info);
    twinvq::Packetizer pkt;
    pkt.frame_bits = info.frame_bits;

    // Streaming output must never truncate the input, including hard links.
    std::error_code path_error;
    if (std::filesystem::equivalent(in_path, out_path, path_error))
        throw std::runtime_error("input and output refer to the same file");
    Wav16Writer wav(out_path, info.sample_rate, info.channels);
    std::vector<float> frame(static_cast<size_t>(info.channels) * info.frame_samples);
    std::vector<uint8_t> file_bytes(static_cast<size_t>(pkt.bytes_to_read()) + 16);
    std::vector<uint8_t> packet(file_bytes.size() + 2);

    double peak = 0;
    uint64_t frames_out = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const int nread = pkt.bytes_to_read();
        file_bytes.resize(static_cast<size_t>(nread));
        in.read(reinterpret_cast<char*>(file_bytes.data()), nread);
        if (in.gcount() < nread)
            break;
        packet.resize(static_cast<size_t>(nread) + 2);
        const int psz = pkt.build(file_bytes.data(), packet.data());
        const int got = dec.decode_packet(packet.data(), psz, frame.data());
        if (got > 0) {
            wav.write(frame.data(), static_cast<size_t>(got) * info.channels);
            frames_out += got;
            for (int i = 0; i < got * info.channels; i++)
                peak = std::max(peak, std::fabs(static_cast<double>(frame[i])));
        }
    }
    wav.finish();
    const auto t1 = std::chrono::steady_clock::now();
    const double decode_s = std::chrono::duration<double>(t1 - t0).count();
    const double audio_s = twinvq::duration_seconds(info);

    std::cout << "decoded     " << frames_out << " samples, peak=" << peak << "\n";
    std::cout << "decode_write_time " << decode_s << " s";
    if (decode_s > 0)
        std::cout << " (" << (audio_s / decode_s) << "x realtime)";
    std::cout << "\n";
    std::cout << "wrote       " << out_path << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
}
