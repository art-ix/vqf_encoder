#include "twinvq/twinvq_encoder.hpp"
#include "twinvq/vqf_file.hpp"
#include "bitstream.hpp"
#include "twinvq_mdct.hpp"
#include "twinvq_window.hpp"
#include "twinvq_psychoacoustic.hpp"
#include "twinvq_tables.hpp"

#include "twinvq_simd.hpp"
#include "twinvq_workers.hpp"
#include <thread>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace twinvq {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float mulawinv(float y, float clip, float mu) {
    y = std::clamp(y / clip, -1.0f, 1.0f);
    const float s = (y < 0) ? -1.0f : 1.0f;
    return clip * s * (std::exp(std::log(1.0f + mu) * std::fabs(y)) - 1.0f) / mu;
}

const std::array<float, 1 << kGainBits>& long_gain_table() {
    static const auto values = [] {
        std::array<float, 1 << kGainBits> result{};
        const float step = kAmpMax / static_cast<float>((1 << kGainBits) - 1);
        for (int q = 0; q < (1 << kGainBits); ++q)
            result[q] = (1.0f / 8192.0f) * mulawinv(step * 0.5f + step * q, kAmpMax, kMulawMu);
        return result;
    }();
    return values;
}

float mulaw(float x, float clip, float mu) {
    x = std::clamp(x, -clip, clip);
    const float s = (x < 0) ? -1.0f : 1.0f;
    return clip * s * std::log(1.0f + mu * std::fabs(x) / clip) / std::log(1.0f + mu);
}

int log2_int(int v) {
    int n = 0;
    while ((1 << (n + 1)) <= v)
        n++;
    return n;
}

int rounded_div(int a, int b) {
    return (a + b / 2) / b;
}

int very_broken_op(int a, int b) {
    int x = a * b + 200;
    if (x % 400 || b % 5)
        return x / 400;
    x /= 400;
    const int idx = b / 5;
    if (idx < 0 || idx >= 13)
        return x;
    const auto& tab = tables::broken_tabs[idx];
    if (!tab.tab || tab.size <= 0)
        return x;
    const int size = tab.size;
    const int l2 = log2_int(std::max(1, 2 * (x - 1) / size));
    return x - tab.tab[size * l2 + (x - 1) % size];
}

void rearrange_lsp(int order, float* lsp, float min_dist) {
    const float min_dist2 = min_dist * 0.5f;
    for (int i = 1; i < order; i++) {
        if (lsp[i] - lsp[i - 1] < min_dist) {
            const float avg = (lsp[i] + lsp[i - 1]) * 0.5f;
            lsp[i - 1] = avg - min_dist2;
            lsp[i] = avg + min_dist2;
        }
    }
}

void sort_floats(float* a, int n) {
    for (int i = 1; i < n; i++) {
        const float x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

float eval_lpc_spectrum(const float* lsp, float cos_val, int order) {
    float p = 0.5f;
    float q = 0.5f;
    const float two_cos_w = 2.0f * cos_val;
    for (int j = 0; j + 1 < order; j += 4) {
        q *= lsp[j] - two_cos_w;
        p *= lsp[j + 1] - two_cos_w;
        q *= lsp[j + 2] - two_cos_w;
        p *= lsp[j + 3] - two_cos_w;
    }
    p *= p * (2.0f - two_cos_w);
    q *= q * (2.0f + two_cos_w);
    return 0.5f / (p + q);
}

void interpolate(float* out, float v1, float v2, int size) {
    const float step = (v1 - v2) / static_cast<float>(size + 1);
    for (int i = 0; i < size; i++) {
        v2 += step;
        out[i] = v2;
    }
}

float get_cos(int idx, int part, const float* cos_tab, int size) {
    return part ? -cos_tab[size - idx - 1] : cos_tab[idx];
}

double cheb_poly(const double* coef, int n, double x) {
    double b0 = 0, b1 = 0;
    for (int i = n; i >= 0; i--) {
        const double b2 = b1;
        b1 = b0;
        b0 = 2.0f * x * b1 - b2 + coef[i];
    }
    return b0 - x * b1;
}

void autocorr(const float* x, int n, float* r, int order) {
    for (int lag = 0; lag <= order; lag++) {
        double s = 0;
        for (int i = 0; i < n - lag; i++)
            s += static_cast<double>(x[i]) * x[i + lag];
        r[lag] = static_cast<float>(s);
    }
    r[0] *= 1.0001f;
    if (r[0] < 1.0e-8f)
        r[0] = 1.0e-8f;
}

void levinson(const float* r, int order, float* a) {
    std::vector<float> tmp(static_cast<size_t>(order) + 1, 0.0f);
    a[0] = 1.0f;
    for (int i = 1; i <= order; i++)
        a[i] = 0.0f;
    float err = r[0];
    for (int i = 1; i <= order; i++) {
        float acc = r[i];
        for (int j = 1; j < i; j++)
            acc += a[j] * r[i - j];
        const float k = (err > 1.0e-12f) ? -acc / err : 0.0f;
        const float kk = std::clamp(k, -0.999f, 0.999f);
        for (int j = 1; j < i; j++)
            tmp[static_cast<size_t>(j)] = a[j] + kk * a[i - j];
        for (int j = 1; j < i; j++)
            a[j] = tmp[static_cast<size_t>(j)];
        a[i] = kk;
        err *= 1.0f - kk * kk;
        if (err < 1.0e-12f)
            err = 1.0e-12f;
    }
}

// Convert LPC (a[0]=1..a[order]) to LSPs in radians (0, pi).
void lpc_to_lsp(const float* a, int order, float* lsp) {
    const int half = order / 2;
    std::vector<double> p(static_cast<size_t>(half) + 1, 0.0);
    std::vector<double> q(static_cast<size_t>(half) + 1, 0.0);
    p[0] = q[0] = 1.0f;
    for (int i = 1; i <= half; i++) {
        p[static_cast<size_t>(i)] = a[i] + a[order + 1 - i] - p[static_cast<size_t>(i - 1)];
        q[static_cast<size_t>(i)] = a[i] - a[order + 1 - i] + q[static_cast<size_t>(i - 1)];
    }
    // The symmetric LPC polynomials are ordered from cos(half*w) down
    // to the constant term. Clenshaw expects the opposite order, and the
    // unpaired constant coefficient has half the weight.
    std::reverse(p.begin(), p.end());
    std::reverse(q.begin(), q.end());
    p[0] *= 0.5;
    q[0] *= 0.5;
    constexpr int ngrid = 1024;
    int found = 0;
    double prev_x = 1.0;
    double prev = cheb_poly(p.data(), half, prev_x);
    for (int g = 1; g <= ngrid && found < order; g++) {
        const double x = std::cos(3.14159265358979323846 * g / ngrid);
        const double* poly = (found & 1) ? q.data() : p.data();
        double value = cheb_poly(poly, half, x);
        // Stable LPC roots alternate between P and Q. Recheck the same
        // interval after each root so close pairs cannot be skipped.
        while (prev * value <= 0 && found < order) {
            double a0 = prev_x, b0 = x, fa = prev;
            for (int it = 0; it < 20; it++) {
                const double m = 0.5 * (a0 + b0);
                const double fm = cheb_poly(poly, half, m);
                if (fa * fm <= 0) {
                    b0 = m;
                } else {
                    a0 = m;
                    fa = fm;
                }
            }
            prev_x = 0.5 * (a0 + b0);
            lsp[found++] = static_cast<float>(std::acos(std::clamp(prev_x, -1.0, 1.0)));
            poly = (found & 1) ? q.data() : p.data();
            prev = cheb_poly(poly, half, prev_x);
            value = cheb_poly(poly, half, x);
        }
        prev_x = x;
        prev = value;
    }
    if (found != order)
        for (int i = 0; i < order; i++)
            lsp[i] = kPi * (static_cast<float>(i) + 1.0f) / static_cast<float>(order + 1);
    rearrange_lsp(order, lsp, 0.001f);
    sort_floats(lsp, order);
}

float vec_err(const float* a, const float* b, int n) {
    double e = 0;
    for (int i = 0; i < n; i++) {
        const double d = static_cast<double>(a[i]) - b[i];
        e += d * d;
    }
    return static_cast<float>(e);
}

int quantize_mu(float linear, float clip, float mu, int bits) {
    const int maxv = (1 << bits) - 1;
    const float step = clip / static_cast<float>(maxv);
    const float y = mulaw(linear, clip, mu);
    // Reconstruction uses (index + 0.5) * step, not index * step.
    const int idx = static_cast<int>(std::floor(y / step));
    return std::clamp(idx, 0, maxv);
}

// Main-codebook vectors live around this RMS (Yamaha 44.1 kHz / 48 kbps/ch
// long frames measure ~6e3). Gain maps flattened MDCT onto that scale.
constexpr float kTargetResidRms = 6000.0f;

// Invert the decoder's conversion from the 16-bit TwinVQ amplitude domain.
constexpr float kMdctPcmScale = 32768.0f;

} // namespace

bool lpc_analysis_self_test(float* max_abs_err) {
    float worst = 0;
    for (int order : {8, 12, 16, 20}) {
        for (int trial = 0; trial < 2; ++trial) {
            float expected[kLspCoefsMax], actual[kLspCoefsMax];
            std::vector<double> p(order + 2), q(order + 2);
            p[0] = q[0] = 1;
            p[1] = 1; q[1] = -1;
            for (int j = 0; j < order; ++j) {
                expected[j] = kPi * (j + 1.0f) / (order + 1.0f);
                if (trial) expected[j] += 0.025f * std::sin(1.7f * j);
                auto& poly = (j & 1) ? q : p;
                const double c = -2 * std::cos(static_cast<double>(expected[j]));
                const auto old = poly;
                for (int i = 0; i < order + 2; ++i)
                    poly[i] = old[i] + (i > 0 ? c * old[i - 1] : 0)
                                      + (i > 1 ? old[i - 2] : 0);
            }
            float a[kLspCoefsMax + 1];
            for (int i = 0; i <= order; ++i) a[i] = static_cast<float>((p[i] + q[i]) * 0.5);
            lpc_to_lsp(a, order, actual);
            for (int i = 0; i < order; ++i)
                worst = std::max(worst, std::fabs(actual[i] - expected[i]));
        }
    }
    if (max_abs_err) *max_abs_err = worst;
    return worst < 1.0e-4f;
}

bool pick_encoder_mode(int sample_rate, int channels, int bitrate_kbps,
                       int& out_rate, int& out_bitrate_kbps, std::string& error) {
    struct Cand {
        int rate;
        int kbps_ch;
    };
    static const Cand kCands[] = {
        {8000, 8},   {11025, 8},  {11025, 10}, {16000, 16}, {22050, 20},
        {22050, 24}, {22050, 32}, {44100, 40}, {44100, 48},
    };
    auto nearest_rate = [](int r) {
        if (r >= 32000)
            return 44100;
        if (r >= 18000)
            return 22050;
        if (r >= 14000)
            return 16000;
        if (r >= 10000)
            return 11025;
        return 8000;
    };
    const int rate = nearest_rate(sample_rate);
    const int ch = std::clamp(channels, 1, 2);
    int best = -1;
    int best_dist = 1 << 30;
    const int want = bitrate_kbps > 0 ? bitrate_kbps : (rate >= 44100 ? 96 : rate >= 22050 ? 64 : 32);
    for (int i = 0; i < 9; i++) {
        if (kCands[i].rate != rate)
            continue;
        const int total = kCands[i].kbps_ch * ch;
        const int dist = std::abs(total - want);
        if (dist < best_dist) {
            best_dist = dist;
            best = total;
        }
    }
    if (best < 0) {
        error = "no TwinVQ mode for this sample rate";
        return false;
    }
    out_rate = rate;
    out_bitrate_kbps = best;
    return true;
}

Encoder::Encoder(const Config& cfg) : cfg_(cfg) {
    if (cfg.threads < 0 || cfg.threads > 32)
        throw std::invalid_argument("threads must be auto (0) or between 1 and 32");
    // Fine-grained searches stop scaling indefinitely. Bound automatic CPU
    // consumption, then reduce further to the available vector groups below.
    if (cfg_.threads == 0)
        cfg_.threads = static_cast<int>(std::max(1u, std::min(8u, std::thread::hardware_concurrency())));
    if ((cfg.simd == Simd::Sse41 && !detail::has_sse41()) ||
        (cfg.simd == Simd::Avx2 && !detail::has_avx2()))
        throw std::invalid_argument("requested SIMD is not supported by this CPU/OS");
    if (cfg.vq_beam != 0 && cfg.vq_beam != 4 && cfg.vq_beam != 8 && cfg.vq_beam != 16 && cfg.vq_beam != 32)
        throw std::invalid_argument("VQ beam must be auto (0), 4, 8, 16 or 32");
    if (cfg.block_mode != BlockMode::Long && cfg.block_mode != BlockMode::Short && cfg.block_mode != BlockMode::Medium && cfg.block_mode != BlockMode::Adaptive)
        throw std::invalid_argument("invalid block mode");
    std::string err;
    int rate = cfg.sample_rate;
    int br = cfg.bitrate_kbps;
    int out_rate = 0, out_br = 0;
    if (!pick_encoder_mode(rate, cfg.channels, br, out_rate, out_br, err))
        throw std::runtime_error(err);
    sample_rate_ = out_rate;
    channels_ = std::clamp(cfg.channels, 1, 2);
    bitrate_kbps_ = out_br;
    if (cfg_.vq_beam == 0)
        cfg_.vq_beam = sample_rate_ == 44100 && channels_ == 2 ? 16 : 4;
    mtab_ = select_mode(sample_rate_, bitrate_kbps_, channels_);
    if (!mtab_)
        throw std::runtime_error("unsupported TwinVQ mode");
    bitrate_bps_ = bitrate_kbps_ * 1000;
    ibps_ = bitrate_kbps_ / channels_;
    isampf_ = (sample_rate_ == 44100) ? 44 : (sample_rate_ == 22050) ? 22 : (sample_rate_ == 11025) ? 11 : sample_rate_ / 1000;
    frame_bits_ = bitrate_bps_ * mtab_->size / sample_rate_;

    overlap_.assign(static_cast<size_t>(channels_) * mtab_->size, 0.0f);
    analysis_window_.resize(static_cast<size_t>(mtab_->size) * 2);
    for (int i = 0; i < mtab_->size * 2; ++i)
        analysis_window_[i] = std::sin((i + 0.5f) * (kPi / (2.0f * mtab_->size)));
    tmp_.assign(static_cast<size_t>(mtab_->size) * 4 + 4096, 0.0f);

    for (int i = 0; i < 3; i++) {
        const int m = 4 * mtab_->size / mtab_->fmode[i].sub;
        const double freq = 2.0 * 3.14159265358979323846 / m;
        cos_tabs_[i].assign(m / 4, 0.0f);
        for (int j = 0; j <= m / 8; j++)
            cos_tabs_[i][j] = static_cast<float>(std::cos((2 * j + 1) * freq));
        for (int j = 1; j < m / 8; j++)
            cos_tabs_[i][m / 4 - j] = cos_tabs_[i][j];
    }

    for (auto& a : bark_hist_)
        for (auto& b : a)
            for (auto& c : b)
                c = 0.1f;

    init_bitstream_params();
    if (cfg_.compensate_delay)
        lead_left_ = 1;
}

void Encoder::init_bitstream_params() {
    const int n_ch = channels_;
    const int total_fr_bits = frame_bits_;
    const int lsp_bits_per_block = n_ch * (mtab_->lsp_bit0 + mtab_->lsp_bit1 + mtab_->lsp_split * mtab_->lsp_bit2);
    const int ppc_bits = n_ch * (mtab_->pgain_bit + mtab_->ppc_shape_bit + mtab_->ppc_period_bit);

    int bse_bits[3];
    int bsize_no_main_cb[3];
    for (int i = 0; i < 3; i++)
        bse_bits[i] = n_ch * (mtab_->fmode[i].bark_n_coef * mtab_->fmode[i].bark_n_bit + 1);

    bsize_no_main_cb[2] = bse_bits[2] + lsp_bits_per_block + ppc_bits + kWindowTypeBits + n_ch * kGainBits;
    for (int i = 0; i < 2; i++)
        bsize_no_main_cb[i] = lsp_bits_per_block + n_ch * kGainBits + kWindowTypeBits +
                              mtab_->fmode[i].sub * (bse_bits[i] + n_ch * kSubGainBits);

    for (int i = 0; i < 4; i++) {
        int bit_size, vect_size;
        if (i == 3) {
            bit_size = n_ch * mtab_->ppc_shape_bit;
            vect_size = n_ch * mtab_->ppc_shape_len;
        } else {
            bit_size = total_fr_bits - bsize_no_main_cb[i];
            vect_size = n_ch * mtab_->size;
        }
        n_div_[i] = (bit_size + 13) / 14;
        const int rounded_up = (bit_size + n_div_[i] - 1) / n_div_[i];
        const int rounded_down = bit_size / n_div_[i];
        const int num_rounded_down = rounded_up * n_div_[i] - bit_size;
        const int num_rounded_up = n_div_[i] - num_rounded_down;
        bits_main_spec_[0][i][0] = static_cast<uint8_t>((rounded_up + 1) / 2);
        bits_main_spec_[1][i][0] = static_cast<uint8_t>(rounded_up / 2);
        bits_main_spec_[0][i][1] = static_cast<uint8_t>((rounded_down + 1) / 2);
        bits_main_spec_[1][i][1] = static_cast<uint8_t>(rounded_down / 2);
        bits_main_spec_change_[i] = num_rounded_up;

        const int vu = (vect_size + n_div_[i] - 1) / n_div_[i];
        const int vd = vect_size / n_div_[i];
        const int nd = vu * n_div_[i] - vect_size;
        const int nu = n_div_[i] - nd;
        length_[i][0] = static_cast<uint8_t>(vu);
        length_[i][1] = static_cast<uint8_t>(vd);
        length_change_[i] = static_cast<uint8_t>(nu);
    }

    for (int ft = 0; ft <= 3; ft++) {
        permut_[ft].assign(4096, 0);
        construct_perm_table(static_cast<FrameType>(ft));
    }
}

void Encoder::construct_perm_table(FrameType ftype) {
    const int fi = static_cast<int>(ftype);
    int size, block_size;
    if (ftype == FrameType::Ppc) {
        size = channels_;
        block_size = mtab_->ppc_shape_len;
    } else {
        size = channels_ * mtab_->fmode[fi].sub;
        block_size = mtab_->size / mtab_->fmode[fi].sub;
    }

    std::vector<int16_t> tmp(4096, 0);
    const int num_vect = n_div_[fi];
    const int num_blocks = size;
    const uint8_t* line_len = length_[fi];
    const int length_div = length_change_[fi];

    for (int i = 0; i < line_len[0]; i++) {
        int shift;
        if (num_blocks == 1 || (ftype == FrameType::Long && num_vect % num_blocks) ||
            (ftype != FrameType::Long && (num_vect & 1)) || i == line_len[1]) {
            shift = 0;
        } else if (ftype == FrameType::Long) {
            shift = i;
        } else {
            shift = i * i;
        }
        for (int j = 0; j < num_vect && (j + num_vect * i < block_size * num_blocks); j++)
            tmp[i * num_vect + j] = static_cast<int16_t>(i * num_vect + (j + shift) % num_vect);
    }

    int cont = 0;
    std::vector<int16_t> trans(4096, 0);
    for (int i = 0; i < num_vect; i++)
        for (int j = 0; j < line_len[i >= length_div]; j++)
            trans[cont++] = tmp[j * num_vect + i];

    const int total = size * block_size;
    for (int i = 0; i < total; i++)
        permut_[fi][i] = static_cast<int16_t>(block_size * (trans[i] % num_blocks) + trans[i] / num_blocks);
}

void Encoder::dequant(const uint8_t* cb_bits, float* out, FrameType ftype,
                      const int16_t* cb0, const int16_t* cb1, int cb_len) {
    const int fi = static_cast<int>(ftype);
    int pos = 0;
    for (int i = 0; i < n_div_[fi]; i++) {
        int sign0 = 1, sign1 = 1;
        const int length = length_[fi][i >= length_change_[fi]];
        const int second = (i >= bits_main_spec_change_[fi]);
        int tmp0 = *cb_bits++;
        if (bits_main_spec_[0][fi][second] == 7) {
            if (tmp0 & 0x40)
                sign0 = -1;
            tmp0 &= 0x3F;
        }
        int tmp1 = *cb_bits++;
        if (bits_main_spec_[1][fi][second] == 7) {
            if (tmp1 & 0x40)
                sign1 = -1;
            tmp1 &= 0x3F;
        }
        const int16_t* tab0 = cb0 + tmp0 * cb_len;
        const int16_t* tab1 = cb1 + tmp1 * cb_len;
        for (int j = 0; j < length; j++)
            out[permut_[fi][pos + j]] = static_cast<float>(sign0 * tab0[j] + sign1 * tab1[j]);
        pos += length;
    }
}

void Encoder::dec_gain(FrameType ftype, float* out) {
    const int sub = mtab_->fmode[static_cast<int>(ftype)].sub;
    const float step = kAmpMax / static_cast<float>((1 << kGainBits) - 1);
    const float sub_step = kSubAmpMax / static_cast<float>((1 << kSubGainBits) - 1);
    if (ftype == FrameType::Long) {
        for (int i = 0; i < channels_; i++)
            out[i] = (1.0f / (1 << 13)) * mulawinv(step * 0.5f + step * gain_bits_[i], kAmpMax, kMulawMu);
    } else {
        for (int i = 0; i < channels_; i++) {
            const float val = (1.0f / (1 << 23)) *
                              mulawinv(step * 0.5f + step * gain_bits_[i], kAmpMax, kMulawMu);
            for (int j = 0; j < sub; j++)
                out[i * sub + j] = val * mulawinv(sub_step * 0.5f + sub_step * sub_gain_bits_[i * sub + j],
                                                  kSubAmpMax, kMulawMu);
        }
    }
}

void Encoder::dec_bark_env(const uint8_t* in, int use_hist, int ch, float* out, float gain, FrameType ftype) {
    const int fi = static_cast<int>(ftype);
    float* hist = bark_hist_[fi][ch];
    const float val = (fi == 0) ? 0.4f : (fi == 1) ? 0.35f : 0.28f;
    const int bark_n_coef = mtab_->fmode[fi].bark_n_coef;
    const int fw_cb_len = mtab_->fmode[fi].bark_env_size / bark_n_coef;
    int idx = 0;
    for (int i = 0; i < fw_cb_len; i++) {
        for (int j = 0; j < bark_n_coef; j++, idx++) {
            const float tmp2 = mtab_->fmode[fi].bark_cb[fw_cb_len * in[j] + i] * (1.0f / 4096.0f);
            float st = use_hist ? (1.0f - val) * tmp2 + val * hist[idx] + 1.0f : tmp2 + 1.0f;
            hist[idx] = tmp2;
            if (st < -1.0f)
                st = 1.0f;
            std::fill_n(out, mtab_->fmode[fi].bark_tab[idx], st * gain);
            out += mtab_->fmode[fi].bark_tab[idx];
        }
    }
}

void Encoder::decode_lsp(int lpc_idx1, const uint8_t* lpc_idx2, int lpc_hist_idx, float* lsp, float* hist) {
    const float* cb = mtab_->lspcodebook;
    const float* cb2 = cb + (1 << mtab_->lsp_bit1) * mtab_->n_lsp;
    const float* cb3 = cb2 + (1 << mtab_->lsp_bit2) * mtab_->n_lsp;
    const int8_t funny[4] = {-2, static_cast<int8_t>(mtab_->lsp_split == 4 ? -2 : 1),
                             static_cast<int8_t>(mtab_->lsp_split == 4 ? -2 : 1), 0};
    int j = 0;
    for (int i = 0; i < mtab_->lsp_split; i++) {
        const int chunk_end = ((i + 1) * mtab_->n_lsp + funny[i]) / mtab_->lsp_split;
        for (; j < chunk_end; j++)
            lsp[j] = cb[lpc_idx1 * mtab_->n_lsp + j] + cb2[lpc_idx2[i] * mtab_->n_lsp + j];
    }
    rearrange_lsp(mtab_->n_lsp, lsp, 0.0001f);
    for (int i = 0; i < mtab_->n_lsp; i++) {
        const float tmp1 = 1.0f - cb3[lpc_hist_idx * mtab_->n_lsp + i];
        const float tmp2 = hist[i] * cb3[lpc_hist_idx * mtab_->n_lsp + i];
        hist[i] = lsp[i];
        lsp[i] = lsp[i] * tmp1 + tmp2;
    }
    rearrange_lsp(mtab_->n_lsp, lsp, 0.0001f);
    rearrange_lsp(mtab_->n_lsp, lsp, 0.000095f);
    sort_floats(lsp, mtab_->n_lsp);
}

void Encoder::eval_lpcenv_or_interp(FrameType ftype, float* out, const float* in, int size, int step, int part) {
    const float* cos_tab = cos_tabs_[static_cast<int>(ftype)].data();
    for (int i = 0; i < size; i += step)
        out[i] = eval_lpc_spectrum(in, get_cos(i, part, cos_tab, size), mtab_->n_lsp);
    for (int i = step; i <= size - 2 * step; i += step) {
        if (out[i + step] + out[i - step] > 1.95f * out[i] || out[i + step] >= out[i - step]) {
            interpolate(out + i - step + 1, out[i], out[i - step], step - 1);
        } else {
            out[i - step / 2] = eval_lpc_spectrum(in, get_cos(i - step / 2, part, cos_tab, size), mtab_->n_lsp);
            interpolate(out + i - step + 1, out[i - step / 2], out[i - step], step / 2 - 1);
            interpolate(out + i - step / 2 + 1, out[i], out[i - step / 2], step / 2 - 1);
        }
    }
    interpolate(out + size - 2 * step + 1, out[size - step], out[size - 2 * step], step - 1);
}

void Encoder::dec_lpc_spectrum_inv(float* lsp, FrameType ftype, float* lpc) {
    const int size = mtab_->size / mtab_->fmode[static_cast<int>(ftype)].sub;
    for (int i = 0; i < mtab_->n_lsp; i++)
        lsp[i] = 2.0f * std::cos(lsp[i]);
    switch (ftype) {
    case FrameType::Long:
        eval_lpcenv_or_interp(ftype, lpc, lsp, size / 2, 8, 0);
        eval_lpcenv_or_interp(ftype, lpc + size / 2, lsp, size / 2, 16, 1);
        interpolate(lpc + size / 2 - 8 + 1, lpc[size / 2], lpc[size / 2 - 8], 8);
        std::fill_n(lpc + size - 16 + 1, 15, lpc[size - 16]);
        break;
    case FrameType::Medium:
        eval_lpcenv_or_interp(ftype, lpc, lsp, size / 2, 2, 0);
        eval_lpcenv_or_interp(ftype, lpc + size / 2, lsp, size / 2, 4, 1);
        interpolate(lpc + size / 2 - 2 + 1, lpc[size / 2], lpc[size / 2 - 2], 2);
        std::fill_n(lpc + size - 4 + 1, 3, lpc[size - 4]);
        break;
    case FrameType::Short: {
        const int size_s = mtab_->size / mtab_->fmode[0].sub;
        for (int i = 0; i < size_s / 2; i++) {
            const float cos_i = cos_tabs_[0][i];
            lpc[i] = eval_lpc_spectrum(lsp, cos_i, mtab_->n_lsp);
            lpc[size_s - i - 1] = eval_lpc_spectrum(lsp, -cos_i, mtab_->n_lsp);
        }
        break;
    }
    default:
        break;
    }
}

void Encoder::decode_ppc(int period_coef, int g_coef, const float* shape, float* speech) {
    const int min_period = rounded_div(40 * 2 * mtab_->size, isampf_);
    const int max_period = rounded_div(40 * 2 * mtab_->size * 6, isampf_);
    const int period_range = max_period - min_period;
    const float pgain_step = 25000.0f / static_cast<float>((1 << mtab_->pgain_bit) - 1);
    const float ppc_gain = 1.0f / 8192.0f *
                           mulawinv(pgain_step * static_cast<float>(g_coef) + pgain_step / 2.0f, 25000.0f, kPgainMu);
    const int period = min_period + rounded_div(period_coef * period_range, (1 << mtab_->ppc_period_bit) - 1);
    int width;
    if (isampf_ == 22 && ibps_ == 32)
        width = rounded_div((period + 800) * mtab_->peak_per2wid, 400 * mtab_->size);
    else
        width = period * mtab_->peak_per2wid / (400 * mtab_->size);

    const int len = mtab_->ppc_shape_len;
    const float* shape_end = shape + len;
    for (int i = 0; i < width / 2 && shape < shape_end; i++)
        speech[i] += ppc_gain * *shape++;
    int i = 1;
    for (; i < rounded_div(len, width); i++) {
        const int center = very_broken_op(period, i);
        for (int j = -width / 2; j < (width + 1) / 2 && shape < shape_end; j++)
            speech[j + center] += ppc_gain * *shape++;
    }
    const int center = very_broken_op(period, i);
    for (int j = -width / 2; j < (width + 1) / 2 && shape < shape_end; j++)
        speech[j + center] += ppc_gain * *shape++;
}

void Encoder::put_bit(unsigned bit) {
    if ((bit_count_ & 7) == 0)
        data_.push_back(0);
    if (bit)
        data_[static_cast<size_t>(bit_count_ >> 3)] |= static_cast<uint8_t>(1u << (7 - (bit_count_ & 7)));
    bit_count_++;
}

void Encoder::put_bits(int n, unsigned v) {
    for (int i = n - 1; i >= 0; i--)
        put_bit((v >> i) & 1u);
}

void Encoder::write_cb_data(const uint8_t* src, FrameType ftype) {
    const int fi = static_cast<int>(ftype);
    for (int i = 0; i < n_div_[fi]; i++) {
        const int second = (i >= bits_main_spec_change_[fi]);
        put_bits(bits_main_spec_[0][fi][second], src[0]);
        put_bits(bits_main_spec_[1][fi][second], src[1]);
        src += 2;
    }
}

void Encoder::write_frame_bits() {
    const int start = bit_count_;
    put_bits(kWindowTypeBits, static_cast<unsigned>(window_type_));
    write_cb_data(main_coeffs_, ftype_);
    const int sub = mtab_->fmode[static_cast<int>(ftype_)].sub;
    for (int i = 0; i < channels_; i++)
        for (int j = 0; j < sub; j++)
            for (int k = 0; k < mtab_->fmode[static_cast<int>(ftype_)].bark_n_coef; k++)
                put_bits(mtab_->fmode[static_cast<int>(ftype_)].bark_n_bit, bark1_[i][j][k]);
    for (int i = 0; i < channels_; i++)
        for (int j = 0; j < sub; j++)
            put_bit(bark_use_hist_[i][j]);
    if (ftype_ == FrameType::Long) {
        for (int i = 0; i < channels_; i++)
            put_bits(kGainBits, gain_bits_[i]);
    } else {
        for (int i = 0; i < channels_; i++) {
            put_bits(kGainBits, gain_bits_[i]);
            for (int j = 0; j < sub; j++)
                put_bits(kSubGainBits, sub_gain_bits_[i * sub + j]);
        }
    }
    for (int i = 0; i < channels_; i++) {
        put_bits(mtab_->lsp_bit0, lpc_hist_idx_[i]);
        put_bits(mtab_->lsp_bit1, lpc_idx1_[i]);
        for (int j = 0; j < mtab_->lsp_split; j++)
            put_bits(mtab_->lsp_bit2, lpc_idx2_[i][j]);
    }
    if (ftype_ == FrameType::Long) {
        write_cb_data(ppc_coeffs_, FrameType::Ppc);
        for (int i = 0; i < channels_; i++) {
            put_bits(mtab_->ppc_period_bit, static_cast<unsigned>(p_coef_[i]));
            put_bits(mtab_->pgain_bit, static_cast<unsigned>(g_coef_[i]));
        }
    }
    if (bit_count_ - start != frame_bits_)
        throw std::runtime_error("incorrect encoded frame bit count");
}

void Encoder::analyze_lpc(const float* time_2n, float* lpc, float* lsp) {
    const int n = mtab_->size;
    const int order = mtab_->n_lsp;
    std::vector<float> win(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n * 2; i++) {
        win[static_cast<size_t>(i)] = time_2n[i] * analysis_window_[i];
    }
    std::vector<float> r(static_cast<size_t>(order) + 1);
    autocorr(win.data(), n * 2, r.data(), order);
    levinson(r.data(), order, lpc);
    lpc_to_lsp(lpc, order, lsp);
}

void Encoder::quantize_lsp(int ch, const float* target_lsp, float* rec_out, LspSearch search) {
    const int order = mtab_->n_lsp;
    const float* cb = mtab_->lspcodebook;
    const float* cb2 = cb + (1 << mtab_->lsp_bit1) * order;
    const int n1 = 1 << mtab_->lsp_bit1;
    const int n2 = 1 << mtab_->lsp_bit2;
    const int n0 = 1 << mtab_->lsp_bit0;
    const int8_t funny[4] = {-2, static_cast<int8_t>(mtab_->lsp_split == 4 ? -2 : 1),
                             static_cast<int8_t>(mtab_->lsp_split == 4 ? -2 : 1), 0};

    int best1 = 0;
    float best_e = 1.0e30f;
    for (int i = 0; i < n1; i++) {
        float e = 0;
        for (int j = 0; j < order; j++) {
            const float d = target_lsp[j] - cb[i * order + j];
            e += d * d;
        }
        if (e < best_e) {
            best_e = e;
            best1 = i;
        }
    }
    lpc_idx1_[ch] = static_cast<uint8_t>(best1);

    std::vector<float> residual(static_cast<size_t>(order));
    for (int j = 0; j < order; j++)
        residual[static_cast<size_t>(j)] = target_lsp[j] - cb[best1 * order + j];

    int j0 = 0;
    for (int s = 0; s < mtab_->lsp_split; s++) {
        const int chunk_end = ((s + 1) * order + funny[s]) / mtab_->lsp_split;
        int best2 = 0;
        best_e = 1.0e30f;
        for (int i = 0; i < n2; i++) {
            float e = 0;
            for (int j = j0; j < chunk_end; j++) {
                const float d = residual[static_cast<size_t>(j)] - cb2[i * order + j];
                e += d * d;
            }
            if (e < best_e) {
                best_e = e;
                best2 = i;
            }
        }
        lpc_idx2_[ch][s] = static_cast<uint8_t>(best2);
        j0 = chunk_end;
    }

    // A separate spectral candidate scores the decoded LPC envelope shape.
    // Remove the mean log ratio because transmitted gain handles overall level.
    // Keep angular search as another full-frame candidate rather than assuming
    // that a smaller envelope distance guarantees better quantized audio.
    constexpr int spectral_bins = 128;
    float target_log[spectral_bins]{}, grid[spectral_bins]{};
    if (search == LspSearch::Spectral) {
        float lsp_cos[kLspCoefsMax];
        for (int j = 0; j < order; ++j) lsp_cos[j] = 2.0f * std::cos(target_lsp[j]);
        for (int k = 0; k < spectral_bins; ++k) {
            grid[k] = std::cos(kPi * (k + 0.5f) / spectral_bins);
            target_log[k] = std::log(std::clamp(
                eval_lpc_spectrum(lsp_cos, grid[k], order), 1.0e-20f, 1.0e20f));
        }
    }
    auto lsp_error = [&](const float* rec) {
        if (search != LspSearch::Spectral) return vec_err(target_lsp, rec, order);
        float lsp_cos[kLspCoefsMax];
        for (int j = 0; j < order; ++j) lsp_cos[j] = 2.0f * std::cos(rec[j]);
        double sum = 0, squares = 0;
        for (int k = 0; k < spectral_bins; ++k) {
            const double d = std::log(std::clamp(
                eval_lpc_spectrum(lsp_cos, grid[k], order), 1.0e-20f, 1.0e20f)) - target_log[k];
            sum += d; squares += d*d;
        }
        return static_cast<float>(std::max(0.0, squares - sum*sum/spectral_bins));
    };

    // History index: try both (lsp_bit0 is 1 → 2 entries) without committing hist.
    float saved_hist[20];
    std::memcpy(saved_hist, lsp_hist_[ch], sizeof(float) * static_cast<size_t>(order));
    int best0 = 0;
    best_e = 1.0e30f;
    for (int h = 0; h < n0; h++) {
        std::memcpy(lsp_hist_[ch], saved_hist, sizeof(float) * static_cast<size_t>(order));
        float rec[kLspCoefsMax];
        decode_lsp(lpc_idx1_[ch], lpc_idx2_[ch], h, rec, lsp_hist_[ch]);
        const float e = lsp_error(rec);
        if (e < best_e) {
            best_e = e;
            best0 = h;
        }
    }
    // Keep the legacy candidate, then search prediction-aware first-stage
    // alternatives. History is private to each trial and committed only once.
    int selected1 = lpc_idx1_[ch];
    uint8_t selected2[kLspSplitMax];
    std::memcpy(selected2, lpc_idx2_[ch], sizeof(selected2));
    const float* predictor = cb2 + n2 * order;
    constexpr int beam = 8;
    for (int h = 0; search != LspSearch::Basic && h < n0; ++h) {
        float errors[beam];
        int indices[beam]{};
        std::fill_n(errors, beam, std::numeric_limits<float>::infinity());
        for (int i = 0; i < n1; ++i) {
            float error = 0;
            for (int j = 0; j < order; ++j) {
                const float history_weight = predictor[h * order + j];
                const float value = (1.0f - history_weight) * cb[i * order + j]
                                  + history_weight * saved_hist[j];
                const float d = target_lsp[j] - value;
                error += d * d;
            }
            for (int slot = 0; slot < beam; ++slot) {
                if (error >= errors[slot]) continue;
                for (int k = beam - 1; k > slot; --k) {
                    errors[k] = errors[k-1]; indices[k] = indices[k-1];
                }
                errors[slot] = error; indices[slot] = i;
                break;
            }
        }
        for (int slot = 0; slot < std::min(beam, n1); ++slot) {
            uint8_t split[kLspSplitMax]{};
            int begin = 0;
            for (int part = 0; part < mtab_->lsp_split; ++part) {
                const int end = ((part + 1) * order + funny[part]) / mtab_->lsp_split;
                float error = std::numeric_limits<float>::infinity();
                for (int i = 0; i < n2; ++i) {
                    float trial = 0;
                    for (int j = begin; j < end; ++j) {
                        const float hw = predictor[h * order + j];
                        const float value = (1.0f - hw) *
                            (cb[indices[slot] * order + j] + cb2[i * order + j]) + hw * saved_hist[j];
                        const float d = target_lsp[j] - value;
                        trial += d*d;
                    }
                    if (trial < error) { error = trial; split[part] = static_cast<uint8_t>(i); }
                }
                begin = end;
            }
            float history[kLspCoefsMax], rec[kLspCoefsMax];
            std::memcpy(history, saved_hist, sizeof(float) * order);
            decode_lsp(indices[slot], split, h, rec, history);
            const float error = lsp_error(rec);
            if (error < best_e) {
                best_e = error; best0 = h; selected1 = indices[slot];
                std::memcpy(selected2, split, sizeof(selected2));
            }
        }
    }
    lpc_idx1_[ch] = static_cast<uint8_t>(selected1);
    std::memcpy(lpc_idx2_[ch], selected2, sizeof(selected2));
    lpc_hist_idx_[ch] = static_cast<uint8_t>(best0);
    std::memcpy(lsp_hist_[ch], saved_hist, sizeof(float) * static_cast<size_t>(order));
    float rec[kLspCoefsMax];
    decode_lsp(lpc_idx1_[ch], lpc_idx2_[ch], best0, rec, lsp_hist_[ch]);
    if (rec_out)
        std::memcpy(rec_out, rec, sizeof(float) * static_cast<size_t>(order));
}

void Encoder::quantize_gain_bark(int ch, const float* spec, int block_size,
                                 const float* lpc_env, bool search, const float* perceptual, int subblock) {
    if (ftype_ == FrameType::Long) {
        double energy = 0;
        for (int i = 0; i < block_size; i++)
            energy += static_cast<double>(spec[i]) * spec[i];
        const float rms = static_cast<float>(std::sqrt(energy / std::max(1, block_size)));

        // Decoder: gain = (1/8192) * mulawinv(q). Set it so spec/gain has codebook RMS.
        const float target_gain = std::max(rms / kTargetResidRms, 1.0e-8f);
        const float target_lin = target_gain * 8192.0f;
        gain_bits_[ch] = static_cast<uint8_t>(quantize_mu(target_lin, kAmpMax, kMulawMu, kGainBits));
    }
    float gtmp[kChannelsMax * kSubblocksMax];
    dec_gain(ftype_, gtmp);
    const int fi = static_cast<int>(ftype_);
    const float gain = gtmp[ch * mtab_->fmode[fi].sub + subblock];
    const int bark_n_coef = mtab_->fmode[fi].bark_n_coef;
    const int fw_cb_len = mtab_->fmode[fi].bark_env_size / bark_n_coef;
    const int n_bit = mtab_->fmode[fi].bark_n_bit;
    const int n_ent = 1 << n_bit;
    std::vector<float> band(static_cast<size_t>(mtab_->fmode[fi].bark_env_size), 0.0f);
    std::vector<double> band_weight(band.size(), 0.0);
    int pos = 0;
    int idx = 0;
    for (int i = 0; i < fw_cb_len; i++) {
        for (int j = 0; j < bark_n_coef; j++, idx++) {
            const int w = mtab_->fmode[fi].bark_tab[idx];
            double s = 0;
            for (int k = 0; k < w && pos + k < block_size; k++)
                s += static_cast<double>(spec[pos + k]) * spec[pos + k];
            // Decoder: bark = (tmp2+1) * gain, spectrum = VQ * bark.
            // VQ lives near kTargetResidRms, so (tmp2+1) should be
            // rms(spec_band) / (gain * kTargetResidRms), not rms/gain.
            const float st = (w > 0 && std::fabs(gain) > 1.0e-12f)
                                 ? static_cast<float>(std::sqrt(s / w)) / (gain * kTargetResidRms)
                                 : 1.0f;
            band[static_cast<size_t>(idx)] = st - 1.0f;
            if (search)
                for (int k = 0; k < w && pos + k < block_size; ++k)
                    band_weight[idx] += static_cast<double>(lpc_env[pos+k]) * lpc_env[pos+k] * perceptual[pos+k];
            pos += w;
        }
    }

    // The legacy fit is kept as a full-frame candidate by encode_frame.
    // Trial history is read-only here; dec_bark_env commits it after selection.
    double best_total = std::numeric_limits<double>::infinity();
    uint8_t selected[kBarkNCoefMax]{};
    int selected_history = 0;
    for (int history = 0; history < (search ? 2 : 1); ++history) {
        uint8_t indices[kBarkNCoefMax]{};
        double total = 0;
        for (int j = 0; j < bark_n_coef; j++) {
            int best = 0;
            double best_e = std::numeric_limits<double>::infinity();
            for (int e = 0; e < n_ent; e++) {
                // Float arithmetic in the legacy path preserves its tie breaks.
                float legacy_error = 0;
                double error = 0;
                int id = j;
                for (int i = 0; i < fw_cb_len; i++, id += bark_n_coef) {
                    const float v = mtab_->fmode[fi].bark_cb[fw_cb_len * e + i] * (1.0f / 4096.0f);
                    if (!search) {
                        const float d = band[id] - v;
                        legacy_error += d*d;
                    } else {
                        const float h = fi == 0 ? 0.4f : fi == 1 ? 0.35f : 0.28f;
                        float st = history ? (1.0f - h) * v + h * bark_hist_[fi][ch][id] + 1.0f : v + 1.0f;
                        if (st < -1.0f) st = 1.0f; // Decoder reconstruction rule.
                        const double d = static_cast<double>(band[id]) + 1.0 - st;
                        error += band_weight[id] * d*d;
                    }
                }
                if (!search) error = legacy_error;
                if (error < best_e) { best_e = error; best = e; }
            }
            indices[j] = static_cast<uint8_t>(best);
            total += best_e;
        }
        if (total < best_total) {
            best_total = total; selected_history = history;
            std::memcpy(selected, indices, sizeof(selected));
        }
    }
    std::memcpy(bark1_[ch][subblock], selected, sizeof(selected));
    bark_use_hist_[ch][subblock] = static_cast<uint8_t>(selected_history);
}

std::vector<int> Encoder::ppc_positions(int period_coef) const {
    const int min_period = rounded_div(40 * 2 * mtab_->size, isampf_);
    const int max_period = rounded_div(40 * 2 * mtab_->size * 6, isampf_);
    const int period = min_period + rounded_div(period_coef * (max_period - min_period),
                                                (1 << mtab_->ppc_period_bit) - 1);
    const int width = isampf_ == 22 && ibps_ == 32
        ? rounded_div((period + 800) * mtab_->peak_per2wid, 400 * mtab_->size)
        : period * mtab_->peak_per2wid / (400 * mtab_->size);
    if (width <= 0) throw std::runtime_error("invalid PPC width");
    const int len = mtab_->ppc_shape_len;
    std::vector<int> positions;
    auto add = [&](int bin) {
        if (static_cast<int>(positions.size()) >= len) return;
        if (bin < 0 || bin >= mtab_->size) throw std::runtime_error("invalid PPC bin");
        positions.push_back(bin);
    };
    for (int i = 0; i < width / 2; ++i) add(i);
    int i = 1;
    for (; i < rounded_div(len, width); ++i) {
        const int center = very_broken_op(period, i);
        for (int j = -width / 2; j < (width + 1) / 2; ++j) add(j + center);
    }
    const int center = very_broken_op(period, i);
    for (int j = -width / 2; j < (width + 1) / 2; ++j) add(j + center);
    if (static_cast<int>(positions.size()) != len) throw std::runtime_error("incomplete PPC map");
    return positions;
}

void Encoder::quantize_ppc(const float* spec, const float* lpc_env, const float* perceptual) {
    const int n = mtab_->size, len = mtab_->ppc_shape_len;
    if (ppc_position_cache_.empty()) {
        for (int p = 0; p < (1 << mtab_->ppc_period_bit); ++p)
            ppc_position_cache_.push_back(ppc_positions(p));
    }
    const float step = 25000.0f / static_cast<float>((1 << mtab_->pgain_bit) - 1);
    std::vector<float> gains(1 << mtab_->pgain_bit);
    for (int q = 0; q < static_cast<int>(gains.size()); ++q)
        gains[q] = (1.0f / 8192.0f) * mulawinv(step * q + step / 2.0f, 25000.0f, kPgainMu);
    // Rank every representable period by reconstructable weighted energy.
    // This is a period proposal; full-frame VQ decides whether it is useful.
    for (int ch = 0; ch < channels_; ++ch) {
        double best = -1;
        for (int p = 0; p < static_cast<int>(ppc_position_cache_.size()); ++p) {
            double energy = 0;
            for (int bin : ppc_position_cache_[p]) {
                const int i = ch * n + bin;
                const double x = static_cast<double>(spec[i]) * lpc_env[i];
                energy += x * x * perceptual[i];
            }
            if (energy > best) { best = energy; p_coef_[ch] = p; }
        }
        double energy = 0;
        for (int bin : ppc_position_cache_[p_coef_[ch]]) {
            const double x = spec[ch * n + bin]; energy += x * x;
        }
        const double target = std::sqrt(energy / len) / kTargetResidRms;
        g_coef_[ch] = 0;
        for (int q = 1; q < static_cast<int>(gains.size()); ++q)
            if (std::fabs(gains[q] - target) < std::fabs(gains[g_coef_[ch]] - target)) g_coef_[ch] = q;
    }
    const int cb_len = (n_div_[3] + len * channels_ - 1) / n_div_[3];
    std::vector<float> target(len * channels_), weights(target.size()), shape(target.size());
    uint8_t best_shape[sizeof(ppc_coeffs_)]{};
    int best_gain[kChannelsMax]{};
    double best_error = std::numeric_limits<double>::infinity();
    for (int pass = 0; pass < 2; ++pass) {
        for (int ch = 0; ch < channels_; ++ch) for (int j = 0; j < len; ++j) {
            const int i = ch * n + ppc_position_cache_[p_coef_[ch]][j];
            const float gain = gains[g_coef_[ch]];
            target[ch * len + j] = spec[i] / gain;
            const float scale = gain * lpc_env[i];
            weights[ch * len + j] = scale * scale * perceptual[i];
        }
        // PPC permutations can mix channels: quantize the entire shape jointly.
        quantize_vectors(target.data(), weights.data(), FrameType::Ppc);
        dequant(ppc_coeffs_, shape.data(), FrameType::Ppc, mtab_->ppc_shape_cb,
                mtab_->ppc_shape_cb + cb_len * kPpcShapeCbSize, cb_len);
        double error = 0;
        for (int ch = 0; ch < channels_; ++ch) {
            double cross = 0, energy = 0;
            for (int j = 0; j < len; ++j) {
                const int i = ch * n + ppc_position_cache_[p_coef_[ch]][j];
                const double w = static_cast<double>(lpc_env[i]) * lpc_env[i] * perceptual[i];
                const double x = shape[ch * len + j];
                cross += w * spec[i] * x; energy += w * x * x;
            }
            const double optimum = energy > 1.0e-20 ? std::max(0.0, cross / energy) : 0;
            g_coef_[ch] = 0;
            for (int q = 1; q < static_cast<int>(gains.size()); ++q)
                if (std::fabs(gains[q] - optimum) < std::fabs(gains[g_coef_[ch]] - optimum)) g_coef_[ch] = q;
            for (int j = 0; j < len; ++j) {
                const int i = ch * n + ppc_position_cache_[p_coef_[ch]][j];
                const double d = (spec[i] - gains[g_coef_[ch]] * shape[ch * len + j]) * lpc_env[i];
                error += perceptual[i] * d * d;
            }
        }
        if (error < best_error) {
            best_error = error;
            std::memcpy(best_shape, ppc_coeffs_, sizeof(ppc_coeffs_));
            std::copy_n(g_coef_, channels_, best_gain);
        }
    }
    std::memcpy(ppc_coeffs_, best_shape, sizeof(ppc_coeffs_));
    std::copy_n(best_gain, channels_, g_coef_);
}

void Encoder::quantize_main(const float* residual, const float* weights) {
    quantize_vectors(residual, weights, ftype_);
}

void Encoder::quantize_vectors(const float* residual, const float* weights, FrameType ftype) {
    const int fi = static_cast<int>(ftype);
    const bool ppc = ftype == FrameType::Ppc;
    const int cb_len = ppc ? (n_div_[3] + mtab_->ppc_shape_len * channels_ - 1) / n_div_[3]
                           : mtab_->fmode[fi].cb_len_read;
    const int16_t* cb0 = ppc ? mtab_->ppc_shape_cb : mtab_->fmode[fi].cb0;
    const int16_t* cb1 = ppc ? cb0 + cb_len * kPpcShapeCbSize : mtab_->fmode[fi].cb1;
    uint8_t* dst = ppc ? ppc_coeffs_ : main_coeffs_;
    detail::VectorError bounded_vector_error = detail::scalar_error;
    detail::CodebookSearch search_codebook = detail::scalar_search;
#if defined(TWINVQ_X86)
    if (cfg_.simd == Simd::Avx2 || (cfg_.simd == Simd::Auto && detail::has_avx2())) {
        bounded_vector_error = detail::avx2_error;
        search_codebook = detail::avx2_search;
    } else if (cfg_.simd == Simd::Sse41 || (cfg_.simd == Simd::Auto && detail::has_sse41())) {
        bounded_vector_error = detail::sse41_error;
        search_codebook = detail::sse41_search;
    }
#endif
    const auto encode_range = [&](int begin, int end) {
        int pos = std::min(begin, static_cast<int>(length_change_[fi])) * length_[fi][0] +
                  std::max(0, begin - static_cast<int>(length_change_[fi])) * length_[fi][1];
        std::vector<float> target(cb_len), weight(cb_len), rest(cb_len);
        for (int i = begin; i < end; i++) {
            const int length = length_[fi][i >= length_change_[fi]];
            const int second = (i >= bits_main_spec_change_[fi]);
            const int bits0 = bits_main_spec_[0][fi][second];
            const int bits1 = bits_main_spec_[1][fi][second];
            const int n0 = (bits0 == 7) ? 64 : (1 << bits0);
            const int n1 = (bits1 == 7) ? 64 : (1 << bits1);
            const bool sign0_en = bits0 == 7;
            const bool sign1_en = bits1 == 7;

            float max_weight = 1.0e-30f;
            for (int j = 0; j < length; j++) {
                target[static_cast<size_t>(j)] = residual[permut_[fi][pos + j]];
                weight[j] = weights[permut_[fi][pos + j]];
                max_weight = std::max(max_weight, weight[j]);
            }
            for (int j = 0; j < length; ++j) weight[j] /= max_weight;

            int best0 = 0, best1 = 0, s0 = 1, s1 = 1;
            float best_e = 1.0e30f;

            // Keep several first-stage choices: the closest cb0 alone need not
            // belong to the best cb0+cb1 pair. Score in reconstructed MDCT units
            // so LPC peaks do not amplify otherwise small quantization errors.
            const int beam_size = cfg_.vq_beam;
            float beam_error[32];
            int beam_index[32]{}, beam_sign[32]{};
            std::fill_n(beam_error, beam_size, 1.0e30f);
            for (int a = 0; a < n0; a++) {
                const int16_t* t0 = cb0 + a * cb_len;
                const int smax = sign0_en ? 2 : 1;
                for (int s = 0; s < smax; s++) {
                    const int sg = (s == 0) ? 1 : -1;
                    const float e = bounded_vector_error(target.data(), weight.data(), t0,
                                                         sg, length, beam_error[beam_size - 1]);
                    for (int slot = 0; slot < beam_size; ++slot) {
                        if (e >= beam_error[slot]) continue;
                        for (int k = beam_size - 1; k > slot; --k) {
                            beam_error[k] = beam_error[k - 1];
                            beam_index[k] = beam_index[k - 1];
                            beam_sign[k] = beam_sign[k - 1];
                        }
                        beam_error[slot] = e; beam_index[slot] = a; beam_sign[slot] = sg;
                        break;
                    }
                }
            }
            auto refine_pair = [&]() {
                for (int pass = 0; pass < 2; ++pass) {
                    for (int stage = 0; stage < 2; ++stage) {
                        const int16_t* fixed = stage ? cb0 + best0 * cb_len : cb1 + best1 * cb_len;
                        const int sign = stage ? s0 : s1;
                        for (int j = 0; j < length; ++j) rest[j] = target[j] - sign * fixed[j];
                        const int16_t* cb = stage ? cb1 : cb0;
                        const auto match = search_codebook(rest.data(), weight.data(), cb,
                            cb_len, length, stage ? n1 : n0, stage ? sign1_en : sign0_en, best_e);
                        if (match.index >= 0) {
                            best_e = match.error;
                            if (stage) { best1 = match.index; s1 = match.sign; }
                            else { best0 = match.index; s0 = match.sign; }
                        }
                    }
                }
            };
            for (int slot = 0; slot < beam_size; ++slot) {
                if (!beam_sign[slot]) continue;
                const int16_t* t0 = cb0 + beam_index[slot] * cb_len;
                for (int j = 0; j < length; ++j) rest[j] = target[j] - beam_sign[slot] * t0[j];
                const auto match = search_codebook(rest.data(), weight.data(), cb1,
                    cb_len, length, n1, sign1_en, best_e);
                if (match.index >= 0) {
                    best_e = match.error;
                    best0 = beam_index[slot]; s0 = beam_sign[slot];
                    best1 = match.index; s1 = match.sign;
                }
                // Refine each prefix of four and retain its winner. A wider beam
                // includes the old four-candidate solution and cannot increase
                // this fixed-target vector error merely by choosing a new seed.
                if ((slot + 1) % 4 != 0) continue;
                refine_pair();

            } // beam candidates and prefix refinement

            // Independently seed from cb1 as well: nearest cb0 entries need not
            // contain the best pair. Eight reverse seeds supplement every beam size.
            // Refine independently, then merge, preserving the forward winner and
            // the smaller-beam inclusion property for fixed targets and weights.
            const int forward0 = best0, forward1 = best1, forward_s0 = s0, forward_s1 = s1;
            const float forward_error = best_e;
            constexpr int reverse_beam = 8;
            float reverse_error[reverse_beam];
            int reverse_index[reverse_beam]{}, reverse_sign[reverse_beam]{};
            std::fill_n(reverse_error, reverse_beam, 1.0e30f);
            for (int b = 0; b < n1; ++b) {
                for (int sign = 0; sign < (sign1_en ? 2 : 1); ++sign) {
                    const int sg = sign ? -1 : 1;
                    const float e = bounded_vector_error(target.data(), weight.data(), cb1 + b * cb_len,
                                                         sg, length, reverse_error[reverse_beam - 1]);
                    for (int slot = 0; slot < reverse_beam; ++slot) {
                        if (e >= reverse_error[slot]) continue;
                        for (int k = reverse_beam - 1; k > slot; --k) {
                            reverse_error[k] = reverse_error[k - 1];
                            reverse_index[k] = reverse_index[k - 1];
                            reverse_sign[k] = reverse_sign[k - 1];
                        }
                        reverse_error[slot] = e; reverse_index[slot] = b; reverse_sign[slot] = sg;
                        break;
                    }
                }
            }
            best_e = 1.0e30f;
            best0 = best1 = 0; s0 = s1 = 1;
            const int reverse_count = std::min(reverse_beam, n1 * (sign1_en ? 2 : 1));
            for (int slot = 0; slot < reverse_count; ++slot) {
                if (!reverse_sign[slot]) continue;
                for (int j = 0; j < length; ++j)
                    rest[j] = target[j] - reverse_sign[slot] * cb1[reverse_index[slot] * cb_len + j];
                const auto match = search_codebook(rest.data(), weight.data(), cb0,
                    cb_len, length, n0, sign0_en, best_e);
                if (match.index >= 0) {
                    best_e = match.error; best0 = match.index; s0 = match.sign;
                    best1 = reverse_index[slot]; s1 = reverse_sign[slot];
                }
                if ((slot + 1) % 4 == 0 || slot + 1 == reverse_count) refine_pair();
            }
            if (forward_error <= best_e) {
                best0 = forward0; best1 = forward1; s0 = forward_s0; s1 = forward_s1;
            }

            uint8_t c0 = static_cast<uint8_t>(best0);
            uint8_t c1 = static_cast<uint8_t>(best1);
            if (sign0_en && s0 < 0)
                c0 |= 0x40;
            if (sign1_en && s1 < 0)
                c1 |= 0x40;
            dst[2 * i] = c0;
            dst[2 * i + 1] = c1;
            pos += length;
        }
    };
    // PPC groups are small. Main-VQ tasks share only immutable inputs and
    // write disjoint coefficient pairs; all workers join before gain fitting.
    const int workers = ppc ? 1 : std::min(cfg_.threads, std::max(1, n_div_[fi] / 16));
    if (workers == 1) encode_range(0, n_div_[fi]);
    else {
        if (!vq_workers_ || vq_workers_->capacity() < workers)
            vq_workers_ = std::make_shared<detail::VqWorkers>(workers - 1);
        // A vector's pruning/search cost varies. Let available workers claim
        // short ranges instead of waiting for a fixed, more expensive partition.
        vq_workers_->run(workers, n_div_[fi], encode_range, 8);
    }
}

// Joint global/sub-gain search in decoder units. For a fixed global gain,
// each sub-gain is independent; all transmitted combinations are considered.
void Encoder::fit_subblock_gains(int ch, const double* target, const double* weight) {
    const int sub = mtab_->fmode[static_cast<int>(ftype_)].sub;
    const float step = kAmpMax / static_cast<float>((1 << kGainBits) - 1);
    const float sub_step = kSubAmpMax / static_cast<float>((1 << kSubGainBits) - 1);
    float sub_values[1 << kSubGainBits];
    for (int q = 0; q < (1 << kSubGainBits); ++q)
        sub_values[q] = mulawinv(sub_step * 0.5f + sub_step * q, kSubAmpMax, kMulawMu);
    double best = std::numeric_limits<double>::infinity();
    for (int g = 0; g < (1 << kGainBits); ++g) {
        const float global = (1.0f / (1 << 23)) * mulawinv(step * 0.5f + step * g, kAmpMax, kMulawMu);
        double error = 0;
        uint8_t selected[kSubblocksMax]{};
        for (int j = 0; j < sub; ++j) {
            double distance = std::numeric_limits<double>::infinity();
            for (int q = 0; q < (1 << kSubGainBits); ++q) {
                const double d = global * sub_values[q] - target[j];
                if (d * d < distance) { distance = d * d; selected[j] = static_cast<uint8_t>(q); }
            }
            error += weight[j] * distance;
        }
        if (error < best) {
            best = error;
            gain_bits_[ch] = static_cast<uint8_t>(g);
            std::copy_n(selected, sub, sub_gain_bits_ + ch * sub);
        }
    }
}

void Encoder::mdct_channel(int ch, const float* time_2n, float* spec_n) {
    const int n = mtab_->size;
    if (cfg_.block_mode != BlockMode::Long) {
        const auto layout = window_layout(*mtab_, ftype_, window_type_);
        const auto next = window_layout(*mtab_, window_frame_type(next_window_type_), next_window_type_);
        std::vector<float> half(n), time(2 * layout.block_size);
        analyze_window_pair(layout, next, time_2n, half.data());
        const int b = layout.block_size;
        const float inverse_scale = -std::sqrt((channels_ == 1 ? 2.0f : 1.0f) / b) / kMdctPcmScale;
        for (int j = 0; j < layout.blocks; ++j) {
            std::fill(time.begin(), time.end(), 0.0f);
            std::copy_n(half.data() + j * b, b, time.data() + b / 2);
            mdct_forward(spec_n + j * b, time.data(), b, 2.0f / (b * inverse_scale));
        }
        return;
    }
    std::vector<float> wbuf(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n * 2; i++) {
        wbuf[static_cast<size_t>(i)] = time_2n[i] * analysis_window_[i];
    }
    const float norm = (channels_ == 1) ? 2.0f : 1.0f;
    const float inv_scale = -kMdctPcmScale / std::sqrt(norm / static_cast<float>(n));
    const float fwd = (2.0f / static_cast<float>(n)) * inv_scale;
    mdct_forward(spec_n, wbuf.data(), n, fwd);
    (void)ch;
}

void Encoder::encode_frame(const float* interleaved_n, bool force_flush, bool next_short) {
    const int n = mtab_->size;
    if (cfg_.block_mode == BlockMode::Adaptive) {
        // This window was promised to the previous frame's analysis. Decide
        // only the next one, using the buffered PCM attack decisions.
        window_type_ = next_window_type_;
        next_window_type_ = force_flush ? 0 : next_short ? 2 : window_type_ == 2 ? 3 : 0;
    } else {
        window_type_ = 0;
        next_window_type_ = 0;
        if (cfg_.block_mode != BlockMode::Long) {
            const bool short_blocks = cfg_.block_mode == BlockMode::Short;
            window_type_ = frames_written_ == 0 ? 0 : force_flush ? (short_blocks ? 3 : 5) : (short_blocks ? 2 : 8);
            next_window_type_ = force_flush ? 0 : (short_blocks ? 2 : 8);
        }
    }
    ftype_ = window_frame_type(window_type_);
    const int sub = mtab_->fmode[static_cast<int>(ftype_)].sub;
    const int block_size = n / sub;
    std::memset(main_coeffs_, 0, sizeof(main_coeffs_));
    std::memset(ppc_coeffs_, 0, sizeof(ppc_coeffs_));
    std::memset(p_coef_, 0, sizeof(p_coef_));
    std::memset(g_coef_, 0, sizeof(g_coef_));

    std::vector<float> ms(static_cast<size_t>(channels_) * n);
    if (channels_ == 2) {
        for (int i = 0; i < n; i++) {
            const float l = interleaved_n[i * 2];
            const float r = interleaved_n[i * 2 + 1];
            // Decoder restores L/R as mid+side / mid-side.
            ms[static_cast<size_t>(i)] = 0.5f * (l + r);
            ms[static_cast<size_t>(n + i)] = 0.5f * (l - r);
        }
    } else {
        std::memcpy(ms.data(), interleaved_n, static_cast<size_t>(n) * sizeof(float));
    }

    // Independent L/R weights on the two PCM hops known to this frame.
    // Emphasize two roughly 3 ms slices preceding a sharp energy rise. Ordinary
    // samples retain unit weight, so the objective still penalizes all error.
    std::vector<float> time_weights;
    if (cfg_.temporal_search) {
        time_weights.assign(channels_ * 2 * n, 1.0f);
        temporal_error_state_.resize(channels_ * 2 * n, 0.0f);
        const int slice = std::max(1, sample_rate_ * 3 / 1000);
        for (int ch = 0; ch < channels_; ++ch) {
            std::vector<double> energy((2 * n + slice - 1) / slice);
            for (int i = 0; i < 2 * n; ++i) {
                const auto& source = i < n ? overlap_ : ms;
                const int pos = i % n;
                const double mid = source[pos];
                const double side = channels_ == 2 ? source[n + pos] : 0;
                const double x = ch == 0 ? mid + side : mid - side;
                energy[i / slice] += x * x;
            }
            double peak = 0;
            for (int j = 0; j < static_cast<int>(energy.size()); ++j) {
                energy[j] /= std::min(slice, 2 * n - j * slice);
                peak = std::max(peak, energy[j]);
            }
            for (int j = 1; j < static_cast<int>(energy.size()); ++j) {
                if (energy[j] <= 8.0 * std::max(energy[j - 1], peak * 1.0e-8) || energy[j] == 0) continue;
                for (int i = std::max(0, (j - 2) * slice); i < j * slice; ++i)
                    time_weights[ch * 2 * n + i] = 4.0f;
            }
        }
    }

    std::vector<float> spec(static_cast<size_t>(channels_) * n, 0.0f);
    std::vector<float> time2n(static_cast<size_t>(n) * 2);
    std::vector<float> target_lsps(static_cast<size_t>(channels_) * kLspCoefsMax, 0.0f);

    for (int ch = 0; ch < channels_; ch++) {
        const float* prev = overlap_.data() + ch * n;
        const float* cur = ms.data() + ch * n;
        for (int i = 0; i < n; i++) {
            time2n[static_cast<size_t>(i)] = prev[i];
            time2n[static_cast<size_t>(n + i)] = cur[i];
        }
        mdct_channel(ch, time2n.data(), spec.data() + ch * n);
        std::memcpy(overlap_.data() + ch * n, cur, static_cast<size_t>(n) * sizeof(float));

        float lpc[kLspCoefsMax + 1];
        float lsp[kLspCoefsMax];
        analyze_lpc(time2n.data(), lpc, lsp);
        std::memcpy(target_lsps.data() + static_cast<size_t>(ch) * kLspCoefsMax, lsp,
                    sizeof(float) * static_cast<size_t>(mtab_->n_lsp));
    }

    std::vector<float> perceptual(spec.size(), 1.0f);
    if (cfg_.psychoacoustic) {
        std::vector<float> block(channels_ * block_size), block_weights(block.size());
        for (int j = 0; j < sub; ++j) {
            for (int ch = 0; ch < channels_; ++ch)
                std::copy_n(spec.data() + ch * n + j * block_size, block_size, block.data() + ch * block_size);
            psychoacoustic_weights(block.data(), block_size, channels_, sample_rate_, block_weights.data());
            for (int ch = 0; ch < channels_; ++ch)
                std::copy_n(block_weights.data() + ch * block_size, block_size, perceptual.data() + ch * n + j * block_size);
        }
    }
    const auto original_spec = spec;
    // Trial encodes share input and prior histories. Snapshot only frame state,
    // never the growing output byte vector or pending track PCM.
    auto snapshot = [](const auto& value) {
        std::array<unsigned char, sizeof(value)> copy;
        std::memcpy(copy.data(), &value, sizeof(value));
        return copy;
    };
    auto restore = [](auto& value, const auto& copy) {
        std::memcpy(&value, copy.data(), sizeof(value));
    };
    const auto prior_lsp = snapshot(lsp_hist_);
    const auto prior_bark = snapshot(bark_hist_);
    using TrialKey = std::array<uint8_t, 1 + kChannelsMax * (2 + kLspSplitMax)>;
    std::vector<TrialKey> evaluated;
    // Different search routes can emit the same initial quantizer state.
    // Skip repeated main VQ only after checking every transmitted envelope
    // field. All trials start from the same history and original spectrum.
    std::vector<std::vector<uint8_t>> prepared_states;
    struct TimeTrial {
        double spectral;
        double temporal;
        std::array<LspSearch, 2> strategy;
        bool bark;
        bool ppc;
    };
    std::vector<TimeTrial> time_trials;
    std::vector<float> trial_error_state;
    auto time_score = [&](const std::vector<float>& delta) {
        const auto layout = window_layout(*mtab_, ftype_, window_type_);
        const auto next = window_layout(*mtab_, window_frame_type(next_window_type_), next_window_type_);
        trial_error_state.assign(channels_ * 2 * n, 0.0f);
        std::vector<float> errors(channels_ * 2 * n), half(n), zero(n), future(2 * n);
        const float scale = -std::sqrt((channels_ == 1 ? 2.0f : 1.0f) / block_size) / kMdctPcmScale;
        for (int ch = 0; ch < channels_; ++ch) {
            for (int j = 0; j < sub; ++j)
                imdct_half(half.data() + j * block_size, delta.data() + ch * n + j * block_size, block_size, scale);
            const float* previous = temporal_error_state_.data() + ch * 2 * n + temporal_error_position_;
            float* current = trial_error_state.data() + ch * 2 * n;
            synthesize_window(layout, half.data(), previous, current);
            const int prefix = n - layout.output_size;
            for (int i = 0; i < n; ++i)
                errors[ch * 2 * n + i] = i < prefix ? previous[i] : current[i - prefix];
            // The next frame's quantization is unknown. Project this frame's
            // tail with zero future error; never alter the persistent state.
            std::fill(future.begin(), future.end(), 0.0f);
            synthesize_window(next, zero.data(), current + layout.output_size, future.data());
            const int next_prefix = n - next.output_size;
            for (int i = 0; i < n; ++i)
                errors[ch * 2 * n + n + i] = i < next_prefix ? current[layout.output_size + i]
                                                                           : future[i - next_prefix];
        }
        double error = 0;
        for (int i = 0; i < 2 * n; ++i) {
            const double mid = errors[i], side = channels_ == 2 ? errors[2 * n + i] : 0;
            error += time_weights[i] * (mid + side) * (mid + side);
            if (channels_ == 2) error += time_weights[2 * n + i] * (mid - side) * (mid - side);
        }
        return error;
    };
    auto commit_time_state = [&]() {
        if (!cfg_.temporal_search) return;
        temporal_error_state_ = trial_error_state;
        temporal_error_position_ = window_layout(*mtab_, ftype_, window_type_).output_size;
    };
    auto trial = [&](const std::array<LspSearch, 2>& strategy, bool bark_search, bool ppc_search = false) {
        restore(lsp_hist_, prior_lsp);
        restore(bark_hist_, prior_bark);
        spec = original_spec;
        std::vector<float> rec_lsps(target_lsps.size());
        for (int ch = 0; ch < channels_; ++ch)
            quantize_lsp(ch, target_lsps.data() + ch * kLspCoefsMax,
                         rec_lsps.data() + ch * kLspCoefsMax, strategy[ch]);
        // Identical transmitted LSP parameters from identical prior histories
        // lead to the same reconstruction. Avoid repeating expensive VQ work.
        TrialKey key{};
        key[0] = static_cast<uint8_t>(bark_search + 2 * ppc_search);
        for (int ch = 0; ch < channels_; ++ch) {
            const int offset = 1 + ch * (2 + kLspSplitMax);
            key[offset] = lpc_idx1_[ch];
            key[offset + 1] = lpc_hist_idx_[ch];
            for (int i = 0; i < mtab_->lsp_split; ++i)
                key[offset + 2 + i] = lpc_idx2_[ch][i];
        }
        if (std::find(evaluated.begin(), evaluated.end(), key) != evaluated.end())
            return std::numeric_limits<double>::infinity();
        evaluated.push_back(key);

        std::vector<float> residual(static_cast<size_t>(channels_) * n);
        std::vector<float> weights(residual.size());
        std::vector<float> scales(cfg_.temporal_search ? residual.size() : 0);
        std::vector<float> offsets(scales.size(), 0.0f);

        std::vector<float> all_env(spec.size(), 1.0f);
        for (int ch = 0; ch < channels_; ch++) {
            float* sp = spec.data() + ch * n;
            float* env = all_env.data() + ch * n;
            float lsp_cos[kLspCoefsMax];
            std::memcpy(lsp_cos, rec_lsps.data() + static_cast<size_t>(ch) * kLspCoefsMax,
                        sizeof(float) * static_cast<size_t>(mtab_->n_lsp));
            dec_lpc_spectrum_inv(lsp_cos, ftype_, env);
            for (int j = 1; j < sub; ++j) std::copy_n(env, block_size, env + j * block_size);
            for (int i = 0; i < n; i++) {
                const float e = std::max(env[static_cast<size_t>(i)], 1.0e-6f);
                sp[i] /= e;
            }
        }
        std::memset(ppc_coeffs_, 0, sizeof(ppc_coeffs_));
        std::memset(p_coef_, 0, sizeof(p_coef_));
        std::memset(g_coef_, 0, sizeof(g_coef_));
        if (ppc_search && ftype_ == FrameType::Long)
            quantize_ppc(spec.data(), all_env.data(), perceptual.data());
        for (int ch = 0; ch < channels_; ++ch) {
            float* sp = spec.data() + ch * n;
            const float* env = all_env.data() + ch * n;
            if (ftype_ == FrameType::Long) {
                const int cb_len_p = (n_div_[3] + mtab_->ppc_shape_len * channels_ - 1) / n_div_[3];
                std::vector<float> ppc_shape(static_cast<size_t>(mtab_->ppc_shape_len) * channels_, 0.0f);
                dequant(ppc_coeffs_, ppc_shape.data(), FrameType::Ppc, mtab_->ppc_shape_cb,
                        mtab_->ppc_shape_cb + cb_len_p * kPpcShapeCbSize, cb_len_p);
                std::vector<float> ppc_add(static_cast<size_t>(n), 0.0f);
                decode_ppc(p_coef_[ch], g_coef_[ch], ppc_shape.data() + ch * mtab_->ppc_shape_len, ppc_add.data());
                for (int i = 0; i < n; i++) {
                    sp[i] -= ppc_add[static_cast<size_t>(i)];
                    if (cfg_.temporal_search) offsets[ch * n + i] = env[i] * ppc_add[i];
                }
            }
            if (sub > 1) {
                double target[kSubblocksMax]{}, importance[kSubblocksMax];
                for (int j = 0; j < sub; ++j) {
                    for (int i = j * block_size; i < (j + 1) * block_size; ++i)
                        target[j] += static_cast<double>(sp[i]) * sp[i];
                    target[j] = std::sqrt(target[j] / block_size) / kTargetResidRms;
                    importance[j] = 1.0;
                }
                fit_subblock_gains(ch, target, importance);
            }
            std::vector<float> bark(static_cast<size_t>(n), 1.0f);
            for (int j = 0; j < sub; ++j) {
                quantize_gain_bark(ch, sp + j * block_size, block_size, env + j * block_size,
                                  bark_search, perceptual.data() + ch * n + j * block_size, j);
                float gain[kChannelsMax * kSubblocksMax];
                dec_gain(ftype_, gain);
                dec_bark_env(bark1_[ch][j], bark_use_hist_[ch][j], ch, bark.data() + j * block_size,
                             gain[ch * sub + j], ftype_);
            }

            float* resid = residual.data() + ch * n;
            for (int i = 0; i < n; i++) {
                const float b = bark[static_cast<size_t>(i)];
                resid[i] = (std::fabs(b) > 1.0e-8f) ? sp[i] / b : sp[i];
                const float synthesis_scale = env[i] * b;
                weights[ch * n + i] = synthesis_scale * synthesis_scale * perceptual[ch * n + i];
                if (cfg_.temporal_search) scales[ch * n + i] = synthesis_scale;
            }
        }

        std::vector<uint8_t> prepared;
        auto append = [&](const auto& field) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&field);
            prepared.insert(prepared.end(), bytes, bytes + sizeof(field));
        };
        append(lpc_idx1_); append(lpc_idx2_); append(lpc_hist_idx_);
        append(bark1_); append(bark_use_hist_); append(gain_bits_); append(sub_gain_bits_);
        append(ppc_coeffs_); append(p_coef_); append(g_coef_);
        if (std::find(prepared_states.begin(), prepared_states.end(), prepared) != prepared_states.end())
            return std::numeric_limits<double>::infinity();
        prepared_states.push_back(std::move(prepared));

        auto finish_trial = [&](double spectral, const float* base) {
            if (!cfg_.temporal_search) return spectral;
            const auto& mode = mtab_->fmode[static_cast<int>(ftype_)];
            std::vector<float> delta(residual.size());
            dequant(main_coeffs_, delta.data(), ftype_, mode.cb0, mode.cb1, mode.cb_len_read);
            float gain[kChannelsMax * kSubblocksMax];
            dec_gain(ftype_, gain);
            for (int k = 0; k < channels_ * sub; ++k) {
                const float ratio = gain[k] / base[k];
                for (int i = k * block_size; i < (k + 1) * block_size; ++i)
                    delta[i] = delta[i] * scales[i] * ratio + offsets[i] - original_spec[i];
            }
            time_trials.push_back({spectral, time_score(delta), strategy, bark_search, ppc_search});
            return spectral;
        };

        if (sub > 1) {
            const auto target = residual;
            const auto base_weights = weights;
            float base[kChannelsMax * kSubblocksMax];
            dec_gain(ftype_, base);
            const auto& mode = mtab_->fmode[static_cast<int>(ftype_)];
            std::vector<float> rec(residual.size());
            double best_error = std::numeric_limits<double>::infinity();
            auto best_main = snapshot(main_coeffs_);
            auto best_gain = snapshot(gain_bits_);
            auto best_sub = snapshot(sub_gain_bits_);
            auto retain = [&]() {
                dequant(main_coeffs_, rec.data(), ftype_, mode.cb0, mode.cb1, mode.cb_len_read);
                float gains[kChannelsMax * kSubblocksMax];
                dec_gain(ftype_, gains);
                double error = 0;
                for (int k = 0; k < channels_ * sub; ++k) {
                    const double ratio = static_cast<double>(gains[k]) / base[k];
                    for (int i = k * block_size; i < (k + 1) * block_size; ++i) {
                        const double d = target[i] - ratio * rec[i];
                        error += base_weights[i] * d * d;
                    }
                }
                if (error < best_error) {
                    best_error = error; best_main = snapshot(main_coeffs_);
                    best_gain = snapshot(gain_bits_); best_sub = snapshot(sub_gain_bits_);
                }
            };
            for (int pass = 0; pass < 2; ++pass) {
                quantize_main(residual.data(), weights.data());
                retain();
                // Fit both gain levels to the selected vectors, including the
                // last pass. Score in the same weighted reconstruction domain.
                for (int ch = 0; ch < channels_; ++ch) {
                    double optimum[kSubblocksMax]{}, importance[kSubblocksMax]{};
                    for (int j = 0; j < sub; ++j) {
                        const int k = ch * sub + j;
                        double cross = 0, energy = 0;
                        for (int i = k * block_size; i < (k + 1) * block_size; ++i) {
                            cross += static_cast<double>(base_weights[i]) * target[i] * rec[i];
                            energy += static_cast<double>(base_weights[i]) * rec[i] * rec[i];
                        }
                        optimum[j] = energy > 1.0e-20 ? base[k] * std::max(0.0, cross / energy) : base[k];
                        importance[j] = energy / (static_cast<double>(base[k]) * base[k]);
                    }
                    fit_subblock_gains(ch, optimum, importance);
                }
                retain();
                restore(main_coeffs_, best_main); restore(gain_bits_, best_gain); restore(sub_gain_bits_, best_sub);
                float gains[kChannelsMax * kSubblocksMax];
                dec_gain(ftype_, gains);
                for (int k = 0; k < channels_ * sub; ++k) {
                    const float ratio = gains[k] / base[k];
                    for (int i = k * block_size; i < (k + 1) * block_size; ++i) {
                        residual[i] = target[i] / ratio;
                        weights[i] = base_weights[i] * ratio * ratio;
                    }
                }
            }
            return finish_trial(best_error, base);
        }

        // All candidates are scored against the same target and synthesis envelope.
        // Keep the previous two-search encoder result among the candidates: a fresh
        // approximate VQ search is not guaranteed to improve on its predecessor.
        const auto target = residual;
        const auto synthesis_weights = weights;
        float base_gain[kChannelsMax * kSubblocksMax];
        dec_gain(FrameType::Long, base_gain);
        uint8_t best_coeffs[sizeof(main_coeffs_)]{};
        uint8_t best_gains[sizeof(gain_bits_)]{};
        double best_error = std::numeric_limits<double>::infinity();
        const auto& main_mode = mtab_->fmode[static_cast<int>(FrameType::Long)];
        std::vector<float> candidate(target.size());
        auto retain_candidate = [&]() {
            dequant(main_coeffs_, candidate.data(), FrameType::Long,
                    main_mode.cb0, main_mode.cb1, main_mode.cb_len_read);
            float gains[kChannelsMax * kSubblocksMax];
            dec_gain(FrameType::Long, gains);
            double error = 0;
            for (int ch = 0; ch < channels_; ++ch) {
                const double ratio = static_cast<double>(gains[ch]) / base_gain[ch];
                for (int i = ch * n; i < (ch + 1) * n; ++i) {
                    const double d = target[i] - ratio * candidate[i];
                    error += synthesis_weights[i] * d * d;
                }
            }
            if (error < best_error) {
                best_error = error;
                std::memcpy(best_coeffs, main_coeffs_, sizeof(main_coeffs_));
                std::memcpy(best_gains, gain_bits_, sizeof(gain_bits_));
            }
        };
        auto restore_best = [&]() {
            std::memcpy(main_coeffs_, best_coeffs, sizeof(main_coeffs_));
            std::memcpy(gain_bits_, best_gains, sizeof(gain_bits_));
        };

        quantize_main(residual.data(), weights.data());
        retain_candidate();
        // Fit the transmitted channel gain to the actual selected vectors.
        // A nominal codebook RMS alone cannot predict the energy of their sum.
        std::vector<float> reconstructed(residual.size());
        const auto& mode = mtab_->fmode[static_cast<int>(FrameType::Long)];
        dequant(main_coeffs_, reconstructed.data(), FrameType::Long, mode.cb0, mode.cb1, mode.cb_len_read);
        float old_gain[kChannelsMax * kSubblocksMax];
        dec_gain(FrameType::Long, old_gain);
        for (int ch = 0; ch < channels_; ++ch) {
            double cross = 0, energy = 0;
            for (int i = ch * n; i < (ch + 1) * n; ++i) {
                cross += static_cast<double>(weights[i]) * residual[i] * reconstructed[i];
                energy += static_cast<double>(weights[i]) * reconstructed[i] * reconstructed[i];
            }
            const float factor = energy > 1.0e-20 ? static_cast<float>(std::max(0.0, cross / energy)) : 1.0f;
            gain_bits_[ch] = static_cast<uint8_t>(quantize_mu(old_gain[ch] * factor * 8192.0f,
                                                           kAmpMax, kMulawMu, kGainBits));
        }
        float new_gain[kChannelsMax * kSubblocksMax];
        dec_gain(FrameType::Long, new_gain);
        for (int ch = 0; ch < channels_; ++ch) {
            const float ratio = new_gain[ch] / old_gain[ch];
            for (int i = ch * n; i < (ch + 1) * n; ++i) {
                residual[i] /= ratio;
                weights[i] *= ratio * ratio;
            }
        }
        // An unchanged decoded gain leaves target and weights unchanged;
        // the deterministic VQ result from the first pass is already present.
        if (!std::equal(old_gain, old_gain + channels_, new_gain))
            quantize_main(residual.data(), weights.data());
        retain_candidate();
        restore_best();

        // Optimize gain for the currently transmitted vectors. This is also
        // used after the last VQ pass, which may have changed those vectors.
        auto fit_candidate_gain = [&]() {
            dequant(main_coeffs_, candidate.data(), FrameType::Long,
                    main_mode.cb0, main_mode.cb1, main_mode.cb_len_read);
            bool changed = false;
            for (int ch = 0; ch < channels_; ++ch) {
                double cross = 0, energy = 0;
                for (int i = ch * n; i < (ch + 1) * n; ++i) {
                    cross += static_cast<double>(synthesis_weights[i]) * target[i] * candidate[i];
                    energy += static_cast<double>(synthesis_weights[i]) * candidate[i] * candidate[i];
                }
                if (energy <= 1.0e-20) continue;
                const double optimum = std::max(0.0, cross / energy);
                int selected = gain_bits_[ch];
                double distance = std::numeric_limits<double>::infinity();
                const auto& decoded_gains = long_gain_table();
                for (int q = 0; q < (1 << kGainBits); ++q) {
                    const float gain = decoded_gains[q];
                    const double d = std::fabs(static_cast<double>(gain) / base_gain[ch] - optimum);
                    if (d < distance) { distance = d; selected = q; }
                }
                changed |= selected != gain_bits_[ch];
                gain_bits_[ch] = static_cast<uint8_t>(selected);
            }
            return changed;
        };

        // Allow one more bounded gain/VQ search when the fitted gain still changes.
        // Retention includes the former two-pass result and its final gain fit.
        for (int pass = 0; pass < 3; ++pass) {
            const double previous_error = best_error;
            const bool changed = fit_candidate_gain();
            if (!changed) break;
            retain_candidate();
            float gains[kChannelsMax * kSubblocksMax];
            dec_gain(FrameType::Long, gains);
            for (int ch = 0; ch < channels_; ++ch) {
                const float ratio = gains[ch] / base_gain[ch];
                for (int i = ch * n; i < (ch + 1) * n; ++i) {
                    residual[i] = target[i] / ratio;
                    weights[i] = synthesis_weights[i] * ratio * ratio;
                }
            }
            quantize_main(residual.data(), weights.data());
            retain_candidate();
            restore_best();
            if (best_error >= previous_error) break;
        }
        // Finish with exact decoded-gain selection, not another VQ search:
        // the final codevectors should never keep a gain fitted to older ones.
        // retain_candidate still guards against floating-point scoring ties.
        if (fit_candidate_gain()) retain_candidate();
        restore_best();
        return finish_trial(best_error, base_gain);
    };

    double selected_error = trial({LspSearch::Basic, LspSearch::Basic}, false);
    if (!cfg_.lsp_search && !cfg_.bark_search && !(cfg_.ppc_search && ftype_ == FrameType::Long)) {
        commit_time_state();
        write_frame_bits();
        frames_written_++;
        return;
    }
    auto selected_main = snapshot(main_coeffs_);
    auto selected_ppc = snapshot(ppc_coeffs_);
    auto selected_period = snapshot(p_coef_);
    auto selected_pgain = snapshot(g_coef_);
    auto selected_gain = snapshot(gain_bits_);
    auto selected_sub_gain = snapshot(sub_gain_bits_);
    auto selected_bark = snapshot(bark1_);
    auto selected_bark_use = snapshot(bark_use_hist_);
    auto selected_lpc1 = snapshot(lpc_idx1_);
    auto selected_lpc2 = snapshot(lpc_idx2_);
    auto selected_lpc_hist = snapshot(lpc_hist_idx_);
    auto selected_lsp_state = snapshot(lsp_hist_);
    auto selected_bark_state = snapshot(bark_hist_);
    // Mid and side can favor different envelopes. Include mixed angular/
    // spectral choices instead of forcing both channels to use one ranking.
    const std::array<LspSearch, 2> strategies[] = {
        {LspSearch::Basic, LspSearch::Basic},
        {LspSearch::Angular, LspSearch::Angular},
        {LspSearch::Spectral, LspSearch::Spectral},
        {LspSearch::Spectral, LspSearch::Angular},
        {LspSearch::Angular, LspSearch::Spectral},
    };
    const int strategy_count = cfg_.lsp_search ? (channels_ == 2 ? 5 : 3) : 1;
    for (int index = 0; index < strategy_count; ++index) {
        for (int bark = 0; bark <= static_cast<int>(cfg_.bark_search); ++bark) {
            for (int ppc = 0; ppc <= static_cast<int>(cfg_.ppc_search && ftype_ == FrameType::Long); ++ppc) {
                if (index == 0 && !bark && !ppc) continue;
                const double error = trial(strategies[index], bark != 0, ppc != 0);
                if (!(error < selected_error)) continue;
                selected_error = error;
                selected_main = snapshot(main_coeffs_);
                selected_ppc = snapshot(ppc_coeffs_);
                selected_period = snapshot(p_coef_);
                selected_pgain = snapshot(g_coef_);
                selected_gain = snapshot(gain_bits_);
                selected_sub_gain = snapshot(sub_gain_bits_);
                selected_bark = snapshot(bark1_);
                selected_bark_use = snapshot(bark_use_hist_);
                selected_lpc1 = snapshot(lpc_idx1_);
                selected_lpc2 = snapshot(lpc_idx2_);
                selected_lpc_hist = snapshot(lpc_hist_idx_);
                selected_lsp_state = snapshot(lsp_hist_);
                selected_bark_state = snapshot(bark_hist_);
            }
        }
    }
    if (cfg_.temporal_search) {
        // Keep a hard per-frame spectral guard relative to the best existing
        // candidate. This is not a whole-track SNR guarantee across histories.
        const TimeTrial* winner = nullptr;
        for (const auto& candidate : time_trials)
            if (candidate.spectral <= selected_error * 1.05 &&
                (!winner || candidate.temporal < winner->temporal)) winner = &candidate;
        if (!winner) throw std::runtime_error("no finite temporal candidate");
        const auto chosen = *winner;
        evaluated.clear();
        prepared_states.clear();
        trial(chosen.strategy, chosen.bark, chosen.ppc); // Regenerate only the selected frame state.
        commit_time_state();
        write_frame_bits();
        frames_written_++;
        return;
    }
    restore(main_coeffs_, selected_main);
    restore(ppc_coeffs_, selected_ppc);
    restore(p_coef_, selected_period);
    restore(g_coef_, selected_pgain);
    restore(gain_bits_, selected_gain);
    restore(sub_gain_bits_, selected_sub_gain);
    restore(bark1_, selected_bark);
    restore(bark_use_hist_, selected_bark_use);
    restore(lpc_idx1_, selected_lpc1);
    restore(lpc_idx2_, selected_lpc2);
    restore(lpc_hist_idx_, selected_lpc_hist);
    restore(lsp_hist_, selected_lsp_state);
    restore(bark_hist_, selected_bark_state);
    write_frame_bits();
    frames_written_++;
}

// Compare short-time energy with a fast-attack, 15 ms release envelope.
// Raising the reference immediately prevents repeated detections while the
// smoothed average is still catching up with an existing attack. First differences
// also catch bright attacks on a sustained bass. Analyze original L/R
// independently so an attack cannot disappear in the mid/side conversion.
// The absolute floor is numerical gating, not a calibrated hearing threshold.
unsigned Encoder::detect_attack(const float* pcm) {
    const int n = mtab_->size;
    const int block = n / mtab_->fmode[static_cast<int>(FrameType::Short)].sub;
    const double release = std::exp(-static_cast<double>(block) / (sample_rate_ * 0.015));
    unsigned attack = 0;
    const int boundary = (n - block / 2) / 2;
    for (int start = 0; start < n; start += block) {
        for (int ch = 0; ch < channels_; ++ch) {
            double energy = 0, high = 0;
            for (int i = start; i < start + block; ++i) {
                const float x = pcm[i * channels_ + ch];
                const double d = static_cast<double>(x) - attack_previous_[ch];
                attack_previous_[ch] = x;
                energy += static_cast<double>(x) * x;
                high += d * d;
            }
            energy /= block;
            high /= block;
            if (energy > 8.0 * std::max(attack_energy_[ch], 1.0e-10)
                || high > 12.0 * std::max(attack_high_energy_[ch], 1.0e-10)) {
                // A Short frame covers the latter part of one PCM hop and
                // the early part of the next. Keep both near the boundary:
                // energy is localized only to this analysis slice.
                if (start < boundary + block) attack |= 1;
                if (start + block > boundary - block) attack |= 2;
            }
            attack_energy_[ch] = std::max(release * attack_energy_[ch], energy);
            attack_high_energy_[ch] = std::max(release * attack_high_energy_[ch], high);
        }
    }
    return attack;
}

void Encoder::submit_hop(const float* pcm, bool final) {
    if (cfg_.block_mode != BlockMode::Adaptive) {
        encode_frame(pcm, final);
        return;
    }
    const unsigned attack = final ? 0 : detect_attack(pcm);
    if (!adaptive_pending_.empty()) {
        // The next frame covers late attacks in the buffered hop and early
        // attacks in the lookahead hop. Close its overlap at EOF as before.
        const bool next_short = (adaptive_attack_ & 2) || (attack & 1);
        encode_frame(adaptive_pending_.data(), false, !final && next_short);
        adaptive_pending_.clear();
    }
    if (final) {
        encode_frame(pcm, true);
    } else {
        adaptive_pending_.assign(pcm, pcm + static_cast<size_t>(mtab_->size) * channels_);
        adaptive_attack_ = attack;
    }
}

void Encoder::feed(const float* interleaved, int frames) {
    if (flushed_)
        throw std::runtime_error("encoder already flushed");
    if (frames < 0 || (frames > 0 && !interleaved))
        throw std::invalid_argument("invalid PCM input");
    if (frames == 0)
        return;
    const int n = mtab_->size;
    const size_t need = static_cast<size_t>(n) * channels_;
    if (lead_left_ > 0) {
        std::vector<float> silence(need, 0.0f);
        while (lead_left_ > 0) {
            submit_hop(silence.data(), false);
            --lead_left_;
        }
    }
    size_t remaining = static_cast<size_t>(frames) * channels_;
    if (!pcm_pending_.empty()) {
        const size_t take = std::min(need - pcm_pending_.size(), remaining);
        pcm_pending_.insert(pcm_pending_.end(), interleaved, interleaved + take);
        interleaved += take;
        remaining -= take;
        if (pcm_pending_.size() == need) {
            submit_hop(pcm_pending_.data(), false);
            pcm_pending_.clear();
        }
    }
    // Process complete hops directly. Erasing the front of a whole-track
    // buffer on every hop used quadratic time and copied gigabytes of PCM.
    while (remaining >= need) {
        submit_hop(interleaved, false);
        interleaved += need;
        remaining -= need;
    }
    pcm_pending_.insert(pcm_pending_.end(), interleaved, interleaved + remaining);
}

void Encoder::flush() {
    if (flushed_)
        return;
    const int n = mtab_->size;
    const int need = n * channels_;
    if (!pcm_pending_.empty()) {
        pcm_pending_.resize(static_cast<size_t>(need), 0.0f);
        submit_hop(pcm_pending_.data(), false);
        pcm_pending_.clear();
    }
    // One extra hop so the last overlap is encoded.
    std::vector<float> z(static_cast<size_t>(need), 0.0f);
    submit_hop(z.data(), true);
    flushed_ = true;
    // Pad DATA to whole bytes.
    const int bits = frames_written_ * frame_bits_;
    const int bytes = (bits + 7) >> 3;
    data_.resize(static_cast<size_t>(bytes), 0);
    vq_workers_.reset(); // No idle workers after the completed stream.
}

VqfInfo Encoder::make_info() const {
    return make_vqf_info(channels_, sample_rate_, bitrate_kbps_, cfg_.tags, data_.size(), cfg_.version);
}

std::vector<uint8_t> Encoder::build_file() const {
    return build_vqf_file(make_info(), data_.data(), data_.size());
}

} // namespace twinvq
