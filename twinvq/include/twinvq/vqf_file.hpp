#pragma once

#include "twinvq_types.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace twinvq {

using ReadFn = std::function<size_t(void* dst, size_t n)>;

// Parse a VQF/TwinVQ header from a sequential reader (starts at file offset 0).
// On success, the reader is positioned at the start of the DATA payload.
bool parse_vqf_header(const ReadFn& read, VqfInfo& info, std::string& error);

// In-memory helper.
bool parse_vqf_header_mem(const uint8_t* data, size_t size, VqfInfo& info, std::string& error);

const ModeTab* select_mode(int sample_rate, int bitrate_kbps, int channels);

// Official NTT TwinVQ / Yamaha SoundVQ modes (kbps per channel). There is no
// 56/64 kbps-per-channel table, so 112/128/160/192 kbps stereo at 44.1 kHz
// is not a legal VQF mode — pick_encoder_mode snaps to 80 or 96.
struct LegalMode {
    int sample_rate;
    int kbps_per_channel;
    int frame_samples;
};
const LegalMode* legal_modes(int& count);


// Rebuild TWIN header + DATA marker (not the audio payload).
std::vector<uint8_t> serialize_vqf_header(const VqfInfo& info);

struct VqfTags {
    std::string title;
    std::string artist;
    std::string comment;
    std::string copyright;
    std::string album;
    std::string genre;
    std::string track;
    std::string year;
    std::string composer;
    std::string publisher;
};

// Replace NAME/AUTH/… chunks. COMM, DSIZ and unknown chunks are kept.
// If strip_all is true, all mapped tag chunks are removed.
void apply_tags(VqfInfo& info, const VqfTags& tags, bool strip_all = false);

VqfChunk make_comm_chunk(int channels, int bitrate_kbps, int sample_rate);
VqfChunk make_dsiz_chunk(uint32_t data_size);

// Build a new VqfInfo (COMM + tags + DSIZ) ready for serialize_vqf_header.
VqfInfo make_vqf_info(int channels, int sample_rate, int bitrate_kbps, const VqfTags& tags,
                      uint64_t data_size, const std::string& version = "97012000");

// TWIN header + DATA payload.
std::vector<uint8_t> build_vqf_file(const VqfInfo& info, const uint8_t* data, size_t data_size);

inline double duration_seconds(const VqfInfo& info) {
    if (info.bitrate_kbps <= 0)
        return 0;
    return (static_cast<double>(info.data_size) * 8.0) / (static_cast<double>(info.bitrate_kbps) * 1000.0);
}

} // namespace twinvq
