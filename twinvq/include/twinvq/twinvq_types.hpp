#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace twinvq {

enum class FrameType { Short = 0, Medium = 1, Long = 2, Ppc = 3 };

constexpr int kChannelsMax = 2;
constexpr int kSubblocksMax = 16;
constexpr int kLspCoefsMax = 20;
constexpr int kLspSplitMax = 4;
constexpr int kBarkNCoefMax = 4;
constexpr int kPpcShapeCbSize = 64;
constexpr int kWindowTypeBits = 4;
constexpr int kGainBits = 8;
constexpr int kSubGainBits = 5;
constexpr float kAmpMax = 13000.0f;
constexpr float kSubAmpMax = 4500.0f;
constexpr float kMulawMu = 100.0f;
constexpr float kPgainMu = 200.0f;

struct VqfChunk {
    char id[4]{};
    std::vector<uint8_t> payload;
};

struct VqfInfo {
    int channels = 0;
    int sample_rate = 0;
    int bitrate_kbps = 0;
    int frame_samples = 0;
    int frame_bits = 0;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    std::string version;
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
    std::vector<VqfChunk> chunks;
};

struct FrameMode {
    uint8_t sub = 0;
    const uint16_t* bark_tab = nullptr;
    uint8_t bark_env_size = 0;
    const int16_t* bark_cb = nullptr;
    uint8_t bark_n_coef = 0;
    uint8_t bark_n_bit = 0;
    const int16_t* cb0 = nullptr;
    const int16_t* cb1 = nullptr;
    uint8_t cb_len_read = 0;
};

struct ModeTab {
    FrameMode fmode[3]{};
    uint16_t size = 0;
    uint8_t n_lsp = 0;
    const float* lspcodebook = nullptr;
    uint8_t lsp_bit0 = 0;
    uint8_t lsp_bit1 = 0;
    uint8_t lsp_bit2 = 0;
    uint8_t lsp_split = 0;
    const int16_t* ppc_shape_cb = nullptr;
    uint8_t ppc_period_bit = 0;
    uint8_t ppc_shape_bit = 0;
    uint8_t ppc_shape_len = 0;
    uint8_t pgain_bit = 0;
    uint16_t peak_per2wid = 0;
};

} // namespace twinvq
