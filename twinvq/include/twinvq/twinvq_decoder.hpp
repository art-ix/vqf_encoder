#pragma once

#include "twinvq_types.hpp"
#include <cstdint>
#include <vector>

namespace twinvq {

// Compare FFT IMDCT against the direct cosine definition.
bool imdct_self_test(float* max_abs_err = nullptr);

class BitReader;

class Decoder {
public:
    explicit Decoder(const VqfInfo& info);

    void reset();

    // Consume one compressed frame from `packet`, including the skip count
    // and leftover-byte prefix written by Packetizer::build().
    // Returns number of interleaved PCM frames written to `out` (0 while priming
    // or on failure). `out` must hold channels * frame_samples floats.
    int decode_packet(const uint8_t* packet, int packet_size, float* out);

    int channels() const { return channels_; }
    int frame_samples() const { return mtab_->size; }
    int frame_bits() const { return frame_bits_; }
    int discarded() const { return discarded_; }

    static int packet_byte_size(int frame_bits, int remaining_bits) {
        return (frame_bits - remaining_bits + 7) >> 3;
    }

private:
    void init_bitstream_params();
    void construct_perm_table(FrameType ftype);
    bool read_bitstream(BitReader& br);
    void dequant(const uint8_t* cb_bits, float* out, FrameType ftype,
                 const int16_t* cb0, const int16_t* cb1, int cb_len);
    void dec_gain(FrameType ftype, float* out);
    void dec_bark_env(const uint8_t* in, int use_hist, int ch, float* out, float gain, FrameType ftype);
    void decode_lsp(int lpc_idx1, const uint8_t* lpc_idx2, int lpc_hist_idx, float* lsp, float* hist);
    void dec_lpc_spectrum_inv(float* lsp, FrameType ftype, float* lpc);
    void decode_ppc(int period_coef, int g_coef, const float* shape, float* speech);
    void read_and_decode_spectrum(float* out, FrameType ftype);
    void imdct_and_window(FrameType ftype, int wtype, float* in, float* prev, int ch);
    void imdct_output(FrameType ftype, int wtype, float* interleaved);
    void eval_lpcenv_or_interp(FrameType ftype, float* out, const float* in, int size, int step, int part);
    void read_cb_data(BitReader& br, uint8_t* dst, FrameType ftype);

    const ModeTab* mtab_ = nullptr;
    int channels_ = 0;
    int sample_rate_ = 0;
    int bitrate_bps_ = 0;
    int ibps_ = 0;
    int isampf_ = 0;
    int frame_bits_ = 0;

    int n_div_[4]{};
    int bits_main_spec_change_[4]{};
    uint8_t bits_main_spec_[2][4][2]{};
    uint8_t length_[4][2]{};
    uint8_t length_change_[4]{};
    std::vector<int16_t> permut_[4];

    float lsp_hist_[2][20]{};
    float bark_hist_[3][2][40]{};

    std::vector<float> spectrum_;
    std::vector<float> curr_frame_;
    std::vector<float> prev_frame_;
    std::vector<float> tmp_buf_;
    std::vector<float> cos_tabs_[3];
    int last_block_pos_[2]{};
    int discarded_ = 0;

    int window_type_ = 0;
    FrameType ftype_ = FrameType::Long;
    uint8_t main_coeffs_[1024]{};
    uint8_t ppc_coeffs_[256]{};
    uint8_t gain_bits_[2]{};
    uint8_t sub_gain_bits_[2 * 16]{};
    uint8_t bark1_[2][16][4]{};
    uint8_t bark_use_hist_[2][16]{};
    uint8_t lpc_idx1_[2]{};
    uint8_t lpc_idx2_[2][4]{};
    uint8_t lpc_hist_idx_[2]{};
    int p_coef_[2]{};
    int g_coef_[2]{};
};

// VQF leftover-bit packetization (one compressed audio frame).
struct Packetizer {
    int frame_bits = 0;
    int remaining_bits = 0;
    uint8_t last_frame_bits = 0;

    int bytes_to_read() const { return Decoder::packet_byte_size(frame_bits, remaining_bits); }

    // `file_bytes` holds `bytes_to_read()` bytes from the DATA stream.
    // Writes size+2 bytes into `packet` (skip count, leftover byte, payload).
    int build(const uint8_t* file_bytes, uint8_t* packet) {
        const int size = bytes_to_read();
        packet[0] = static_cast<uint8_t>(8 - remaining_bits);
        packet[1] = last_frame_bits;
        for (int i = 0; i < size; i++)
            packet[2 + i] = file_bytes[i];
        last_frame_bits = packet[size + 1];
        remaining_bits = (size << 3) - frame_bits + remaining_bits;
        return size + 2;
    }

    void seek_prep(int64_t bit_pos) {
        last_frame_bits = 0;
        // Bit 0 has no previous leftover byte. The (pos-7) formula seeks one
        // byte before DATA and skips it; if the offset is clamped to DATA
        // start, remaining_bits=-8 skips the first audio byte and desyncs.
        if (bit_pos <= 0) {
            remaining_bits = 0;
            return;
        }
        remaining_bits = static_cast<int>(-7 - ((bit_pos - 7) & 7));
    }

    static int64_t file_offset_for_bit(int64_t bit_pos, uint64_t data_offset) {
        if (bit_pos <= 0)
            return static_cast<int64_t>(data_offset);
        const int64_t rel = (bit_pos - 7) >> 3;
        if (rel < 0)
            return static_cast<int64_t>(data_offset);
        return static_cast<int64_t>(data_offset) + rel;
    }
};

} // namespace twinvq
