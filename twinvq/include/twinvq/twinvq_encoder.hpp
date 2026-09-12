#pragma once

#include "twinvq_types.hpp"
#include "vqf_file.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace twinvq {

// Verify LPC -> LSP analysis against known stable predictor polynomials.
bool lpc_analysis_self_test(float* max_abs_err);

class Encoder {
public:
    struct Config {
        int sample_rate = 44100;
        int channels = 2;
        int bitrate_kbps = 96; // total
        VqfTags tags;
        std::string version = "97012000";
        // Prepend one hop; MDCT overlap supplies the other decoder priming hop.
        bool compensate_delay = true;
        // Experimental frame-scored LSP beam search; increases encode time.
        bool lsp_search = false;
        // Experimental reconstruction-weighted Bark/history candidate.
        bool bark_search = false;
    };

    explicit Encoder(const Config& cfg);

    int channels() const { return channels_; }
    int sample_rate() const { return sample_rate_; }
    int bitrate_kbps() const { return bitrate_kbps_; }
    int frame_samples() const { return mtab_->size; }
    int frame_bits() const { return frame_bits_; }
    const ModeTab* mode() const { return mtab_; }

    // Interleaved float PCM in [-1, 1]. May buffer internally.
    void feed(const float* interleaved, int frames);

    // Flush MDCT overlap + delay compensation. Call once at the end.
    void flush();

    // Packed DATA bitstream (not including the VQF header).
    const std::vector<uint8_t>& data() const { return data_; }
    uint64_t data_size() const { return data_.size(); }
    int frames_written() const { return frames_written_; }

    VqfInfo make_info() const;
    std::vector<uint8_t> build_file() const;

private:
    void init_bitstream_params();
    void construct_perm_table(FrameType ftype);
    void encode_frame(const float* interleaved_n, bool force_flush);
    void mdct_channel(int ch, const float* time_2n, float* spec_n);
    void analyze_lpc(const float* time_n, float* lpc, float* lsp);
    void quantize_lsp(int ch, const float* target_lsp, float* rec_out, bool search);
    void quantize_gain_bark(int ch, const float* spec, int block_size,
                            const float* lpc_env, bool search);
    void quantize_ppc(int ch, float* spec);
    void quantize_main(const float* residual, const float* weights);
    void write_frame_bits();

    // Decoder-identical helpers so envelopes match on the far side.
    void dequant(const uint8_t* cb_bits, float* out, FrameType ftype,
                 const int16_t* cb0, const int16_t* cb1, int cb_len);
    void dec_gain(FrameType ftype, float* out);
    void dec_bark_env(const uint8_t* in, int use_hist, int ch, float* out, float gain, FrameType ftype);
    void decode_lsp(int lpc_idx1, const uint8_t* lpc_idx2, int lpc_hist_idx, float* lsp, float* hist);
    void dec_lpc_spectrum_inv(float* lsp, FrameType ftype, float* lpc);
    void decode_ppc(int period_coef, int g_coef, const float* shape, float* speech);
    void eval_lpcenv_or_interp(FrameType ftype, float* out, const float* in, int size, int step, int part);
    void write_cb_data(const uint8_t* src, FrameType ftype);

    const ModeTab* mtab_ = nullptr;
    Config cfg_{};
    int channels_ = 0;
    int sample_rate_ = 0;
    int bitrate_kbps_ = 0;
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

    std::vector<float> overlap_;      // channels * N previous samples (mid/side)
    std::vector<float> pcm_pending_;  // interleaved leftover input
    std::vector<float> analysis_window_;
    int lead_left_ = 0;
    bool flushed_ = false;

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

    std::vector<float> cos_tabs_[3];
    std::vector<float> tmp_;
    std::vector<uint8_t> data_;
    int frames_written_ = 0;
    int bit_count_ = 0;

    void put_bits(int n, unsigned v);
    void put_bit(unsigned bit);
};

// Pick a supported TwinVQ mode for (rate, channels). Prefers the highest
// bitrate at that rate when `bitrate_kbps` is 0 or not an exact mode.
// 128 kbps stereo is not legal (no 64 kbps/ch table); 96 is the 44.1 kHz max.
bool pick_encoder_mode(int sample_rate, int channels, int bitrate_kbps,
                       int& out_rate, int& out_bitrate_kbps, std::string& error);

} // namespace twinvq
