#include "twinvq/twinvq_encoder.hpp"
#include "twinvq/vqf_file.hpp"
#include "bitstream.hpp"
#include "twinvq_mdct.hpp"
#include "twinvq_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace twinvq {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float mulawinv(float y, float clip, float mu) {
    y = std::clamp(y / clip, -1.0f, 1.0f);
    const float s = (y < 0) ? -1.0f : 1.0f;
    return clip * s * (std::exp(std::log(1.0f + mu) * std::fabs(y)) - 1.0f) / mu;
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

float cheb_poly(const float* coef, int n, float x) {
    float b0 = 0, b1 = 0;
    for (int i = n; i >= 0; i--) {
        const float b2 = b1;
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
    std::vector<float> p(static_cast<size_t>(half) + 1, 0.0f);
    std::vector<float> q(static_cast<size_t>(half) + 1, 0.0f);
    p[0] = q[0] = 1.0f;
    for (int i = 1; i <= half; i++) {
        p[static_cast<size_t>(i)] = a[i] + a[order + 1 - i] - p[static_cast<size_t>(i - 1)];
        q[static_cast<size_t>(i)] = a[i] - a[order + 1 - i] + q[static_cast<size_t>(i - 1)];
    }
    const int ngrid = 256;
    int found = 0;
    float prev_x = 1.0f;
    float prev_p = cheb_poly(p.data(), half, prev_x);
    float prev_q = cheb_poly(q.data(), half, prev_x);
    for (int g = 1; g <= ngrid && found < order; g++) {
        const float x = std::cos(kPi * static_cast<float>(g) / static_cast<float>(ngrid));
        const float pv = cheb_poly(p.data(), half, x);
        const float qv = cheb_poly(q.data(), half, x);
        if (prev_p * pv <= 0.0f && found < order) {
            float a0 = prev_x, b0 = x, fa = prev_p;
            for (int it = 0; it < 8; it++) {
                const float m = 0.5f * (a0 + b0);
                const float fm = cheb_poly(p.data(), half, m);
                if (fa * fm <= 0) {
                    b0 = m;
                } else {
                    a0 = m;
                    fa = fm;
                }
            }
            lsp[found++] = std::acos(std::clamp(0.5f * (a0 + b0), -1.0f, 1.0f));
        }
        if (prev_q * qv <= 0.0f && found < order) {
            float a0 = prev_x, b0 = x, fa = prev_q;
            for (int it = 0; it < 8; it++) {
                const float m = 0.5f * (a0 + b0);
                const float fm = cheb_poly(q.data(), half, m);
                if (fa * fm <= 0) {
                    b0 = m;
                } else {
                    a0 = m;
                    fa = fm;
                }
            }
            lsp[found++] = std::acos(std::clamp(0.5f * (a0 + b0), -1.0f, 1.0f));
        }
        prev_x = x;
        prev_p = pv;
        prev_q = qv;
    }
    sort_floats(lsp, found);
    for (int i = found; i < order; i++)
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
    int idx = static_cast<int>(std::floor((y / step) - 0.5f + 0.5f));
    return std::clamp(idx, 0, maxv);
}

} // namespace

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
    std::string err;
    int rate = cfg.sample_rate;
    int br = cfg.bitrate_kbps;
    int out_rate = 0, out_br = 0;
    if (!pick_encoder_mode(rate, cfg.channels, br, out_rate, out_br, err))
        throw std::runtime_error(err);
    sample_rate_ = out_rate;
    channels_ = std::clamp(cfg.channels, 1, 2);
    bitrate_kbps_ = out_br;
    mtab_ = select_mode(sample_rate_, bitrate_kbps_, channels_);
    if (!mtab_)
        throw std::runtime_error("unsupported TwinVQ mode");
    bitrate_bps_ = bitrate_kbps_ * 1000;
    ibps_ = bitrate_kbps_ / channels_;
    isampf_ = (sample_rate_ == 44100) ? 44 : (sample_rate_ == 22050) ? 22 : (sample_rate_ == 11025) ? 11 : sample_rate_ / 1000;
    frame_bits_ = bitrate_bps_ * mtab_->size / sample_rate_;

    overlap_.assign(static_cast<size_t>(channels_) * mtab_->size, 0.0f);
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
        lead_left_ = 2;
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
    while (bit_count_ - start < frame_bits_)
        put_bit(0);
}

void Encoder::analyze_lpc(const float* time_n, float* lpc, float* lsp) {
    const int n = mtab_->size;
    const int order = mtab_->n_lsp;
    std::vector<float> win(static_cast<size_t>(n));
    const float* sw = sine_window_cached(n);
    for (int i = 0; i < n; i++)
        win[static_cast<size_t>(i)] = time_n[i] * sw[i];
    std::vector<float> r(static_cast<size_t>(order) + 1);
    autocorr(win.data(), n, r.data(), order);
    levinson(r.data(), order, lpc);
    lpc_to_lsp(lpc, order, lsp);
}

void Encoder::quantize_lsp(int ch, const float* target_lsp, float* rec_out) {
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

    // History index: try both (lsp_bit0 is 1 → 2 entries) without committing hist.
    float saved_hist[20];
    std::memcpy(saved_hist, lsp_hist_[ch], sizeof(float) * static_cast<size_t>(order));
    int best0 = 0;
    best_e = 1.0e30f;
    for (int h = 0; h < n0; h++) {
        std::memcpy(lsp_hist_[ch], saved_hist, sizeof(float) * static_cast<size_t>(order));
        float rec[kLspCoefsMax];
        decode_lsp(lpc_idx1_[ch], lpc_idx2_[ch], h, rec, lsp_hist_[ch]);
        const float e = vec_err(target_lsp, rec, order);
        if (e < best_e) {
            best_e = e;
            best0 = h;
        }
    }
    lpc_hist_idx_[ch] = static_cast<uint8_t>(best0);
    std::memcpy(lsp_hist_[ch], saved_hist, sizeof(float) * static_cast<size_t>(order));
    float rec[kLspCoefsMax];
    decode_lsp(lpc_idx1_[ch], lpc_idx2_[ch], best0, rec, lsp_hist_[ch]);
    if (rec_out)
        std::memcpy(rec_out, rec, sizeof(float) * static_cast<size_t>(order));
}

void Encoder::quantize_gain_bark(int ch, const float* spec, int block_size) {
    // Long frames only in this encoder: one gain, one bark set.
    double energy = 0;
    for (int i = 0; i < block_size; i++)
        energy += static_cast<double>(spec[i]) * spec[i];
    const float rms = static_cast<float>(std::sqrt(energy / std::max(1, block_size)));

    // Decoder: gain = (1/8192) * mulawinv(q). Invert for a first guess.
    const float target_lin = rms * 8192.0f;
    gain_bits_[ch] = static_cast<uint8_t>(quantize_mu(target_lin, kAmpMax, kMulawMu, kGainBits));

    float gtmp[kChannelsMax * kSubblocksMax];
    // Peek decoded gain without destroying other channels' bits: only this ch matters for long.
    dec_gain(FrameType::Long, gtmp);
    const float gain = gtmp[ch];
    const float inv_g = (std::fabs(gain) > 1.0e-12f) ? 1.0f / gain : 1.0f;

    const int fi = static_cast<int>(FrameType::Long);
    const int bark_n_coef = mtab_->fmode[fi].bark_n_coef;
    const int fw_cb_len = mtab_->fmode[fi].bark_env_size / bark_n_coef;
    const int n_bit = mtab_->fmode[fi].bark_n_bit;
    const int n_ent = 1 << n_bit;

    // Band means of |spec| / gain  (want st ≈ band / 1 after +1).
    std::vector<float> band(static_cast<size_t>(mtab_->fmode[fi].bark_env_size), 0.0f);
    int pos = 0;
    int idx = 0;
    for (int i = 0; i < fw_cb_len; i++) {
        for (int j = 0; j < bark_n_coef; j++, idx++) {
            const int w = mtab_->fmode[fi].bark_tab[idx];
            double s = 0;
            for (int k = 0; k < w && pos + k < block_size; k++)
                s += std::fabs(spec[pos + k]);
            const float mean = (w > 0) ? static_cast<float>(s / w) * inv_g : 1.0f;
            band[static_cast<size_t>(idx)] = mean - 1.0f; // tmp2 target
            pos += w;
        }
    }

    for (int j = 0; j < bark_n_coef; j++) {
        int best = 0;
        float best_e = 1.0e30f;
        for (int e = 0; e < n_ent; e++) {
            float err = 0;
            int id = j;
            for (int i = 0; i < fw_cb_len; i++, id += bark_n_coef) {
                const float v = mtab_->fmode[fi].bark_cb[fw_cb_len * e + i] * (1.0f / 4096.0f);
                const float d = band[static_cast<size_t>(id)] - v;
                err += d * d;
            }
            if (err < best_e) {
                best_e = err;
                best = e;
            }
        }
        bark1_[ch][0][j] = static_cast<uint8_t>(best);
    }
    bark_use_hist_[ch][0] = 0;
}

void Encoder::quantize_ppc(int ch, float* spec) {
    // PPC shape VQ is not yet a full two-stage search; send silence so the
    // decoder does not inject codebook[0] * estimated gain.
    (void)spec;
    p_coef_[ch] = 0;
    g_coef_[ch] = 0;
}

void Encoder::quantize_main(const float* residual) {
    const FrameType ftype = FrameType::Long;
    const int fi = static_cast<int>(ftype);
    const int16_t* cb0 = mtab_->fmode[fi].cb0;
    const int16_t* cb1 = mtab_->fmode[fi].cb1;
    const int cb_len = mtab_->fmode[fi].cb_len_read;
    uint8_t* dst = main_coeffs_;
    int pos = 0;
    for (int i = 0; i < n_div_[fi]; i++) {
        const int length = length_[fi][i >= length_change_[fi]];
        const int second = (i >= bits_main_spec_change_[fi]);
        const int bits0 = bits_main_spec_[0][fi][second];
        const int bits1 = bits_main_spec_[1][fi][second];
        const int n0 = (bits0 == 7) ? 64 : (1 << bits0);
        const int n1 = (bits1 == 7) ? 64 : (1 << bits1);
        const bool sign0_en = bits0 == 7;
        const bool sign1_en = bits1 == 7;

        std::vector<float> target(static_cast<size_t>(length));
        for (int j = 0; j < length; j++)
            target[static_cast<size_t>(j)] = residual[permut_[fi][pos + j]];

        int best0 = 0, best1 = 0, s0 = 1, s1 = 1;
        float best_e = 1.0e30f;

        // Stage 1: cb0
        float best_stage = 1.0e30f;
        for (int a = 0; a < n0; a++) {
            const int16_t* t0 = cb0 + a * cb_len;
            const int smax = sign0_en ? 2 : 1;
            for (int s = 0; s < smax; s++) {
                const int sg = (s == 0) ? 1 : -1;
                float e = 0;
                for (int j = 0; j < length; j++) {
                    const float d = target[static_cast<size_t>(j)] - sg * t0[j];
                    e += d * d;
                }
                if (e < best_stage) {
                    best_stage = e;
                    best0 = a;
                    s0 = sg;
                }
            }
        }
        const int16_t* t0 = cb0 + best0 * cb_len;
        best_e = 1.0e30f;
        for (int b = 0; b < n1; b++) {
            const int16_t* t1 = cb1 + b * cb_len;
            const int smax = sign1_en ? 2 : 1;
            for (int s = 0; s < smax; s++) {
                const int sg = (s == 0) ? 1 : -1;
                float e = 0;
                for (int j = 0; j < length; j++) {
                    const float d = target[static_cast<size_t>(j)] - s0 * t0[j] - sg * t1[j];
                    e += d * d;
                }
                if (e < best_e) {
                    best_e = e;
                    best1 = b;
                    s1 = sg;
                }
            }
        }

        uint8_t c0 = static_cast<uint8_t>(best0);
        uint8_t c1 = static_cast<uint8_t>(best1);
        if (sign0_en && s0 < 0)
            c0 |= 0x40;
        if (sign1_en && s1 < 0)
            c1 |= 0x40;
        *dst++ = c0;
        *dst++ = c1;
        pos += length;
    }
}

void Encoder::mdct_channel(int ch, const float* time_2n, float* spec_n) {
    const int n = mtab_->size;
    std::vector<float> wbuf(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n * 2; i++) {
        const float w = std::sin((static_cast<float>(i) + 0.5f) * (kPi / (2.0f * static_cast<float>(n))));
        wbuf[static_cast<size_t>(i)] = time_2n[i] * w;
    }
    const float norm = (channels_ == 1) ? 2.0f : 1.0f;
    const float inv_scale = -32768.0f / std::sqrt(norm / static_cast<float>(n));
    const float fwd = (2.0f / static_cast<float>(n)) * inv_scale;
    mdct_forward(spec_n, wbuf.data(), n, fwd);
    (void)ch;
}

void Encoder::encode_frame(const float* interleaved_n, bool /*force_flush*/) {
    const int n = mtab_->size;
    window_type_ = 0;
    ftype_ = FrameType::Long;
    std::memset(main_coeffs_, 0, sizeof(main_coeffs_));
    std::memset(ppc_coeffs_, 0, sizeof(ppc_coeffs_));
    std::memset(p_coef_, 0, sizeof(p_coef_));
    std::memset(g_coef_, 0, sizeof(g_coef_));

    std::vector<float> ms(static_cast<size_t>(channels_) * n);
    if (channels_ == 2) {
        for (int i = 0; i < n; i++) {
            const float l = interleaved_n[i * 2];
            const float r = interleaved_n[i * 2 + 1];
            ms[static_cast<size_t>(i)] = 0.5f * (l + r);
            ms[static_cast<size_t>(n + i)] = 0.5f * (l - r);
        }
    } else {
        std::memcpy(ms.data(), interleaved_n, static_cast<size_t>(n) * sizeof(float));
    }

    std::vector<float> spec(static_cast<size_t>(channels_) * n, 0.0f);
    std::vector<float> time2n(static_cast<size_t>(n) * 2);
    std::vector<float> rec_lsps(static_cast<size_t>(channels_) * kLspCoefsMax, 0.0f);

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
        float rec_lsp[kLspCoefsMax];
        analyze_lpc(cur, lpc, lsp);
        quantize_lsp(ch, lsp, rec_lsp);
        std::memcpy(rec_lsps.data() + static_cast<size_t>(ch) * kLspCoefsMax, rec_lsp,
                    sizeof(float) * static_cast<size_t>(mtab_->n_lsp));
    }

    std::vector<float> residual(static_cast<size_t>(channels_) * n);

    for (int ch = 0; ch < channels_; ch++) {
        float* sp = spec.data() + ch * n;
        std::vector<float> env(static_cast<size_t>(n), 1.0f);
        float lsp_cos[kLspCoefsMax];
        std::memcpy(lsp_cos, rec_lsps.data() + static_cast<size_t>(ch) * kLspCoefsMax,
                    sizeof(float) * static_cast<size_t>(mtab_->n_lsp));
        dec_lpc_spectrum_inv(lsp_cos, FrameType::Long, env.data());
        for (int i = 0; i < n; i++) {
            const float e = std::max(env[static_cast<size_t>(i)], 1.0e-6f);
            sp[i] /= e;
        }

        quantize_ppc(ch, sp);

        const int cb_len_p = (n_div_[3] + mtab_->ppc_shape_len * channels_ - 1) / n_div_[3];
        std::vector<float> ppc_shape(static_cast<size_t>(mtab_->ppc_shape_len) * channels_, 0.0f);
        dequant(ppc_coeffs_, ppc_shape.data(), FrameType::Ppc, mtab_->ppc_shape_cb,
                mtab_->ppc_shape_cb + cb_len_p * kPpcShapeCbSize, cb_len_p);
        std::vector<float> ppc_add(static_cast<size_t>(n), 0.0f);
        decode_ppc(p_coef_[ch], g_coef_[ch], ppc_shape.data() + ch * mtab_->ppc_shape_len, ppc_add.data());
        for (int i = 0; i < n; i++)
            sp[i] -= ppc_add[static_cast<size_t>(i)];

        quantize_gain_bark(ch, sp, n);

        float gain[kChannelsMax * kSubblocksMax];
        dec_gain(FrameType::Long, gain);
        std::vector<float> bark(static_cast<size_t>(n), 1.0f);
        dec_bark_env(bark1_[ch][0], bark_use_hist_[ch][0], ch, bark.data(), gain[ch], FrameType::Long);
        float* resid = residual.data() + ch * n;
        for (int i = 0; i < n; i++) {
            const float b = bark[static_cast<size_t>(i)];
            resid[i] = (std::fabs(b) > 1.0e-8f) ? sp[i] / b : sp[i];
        }
    }

    quantize_main(residual.data());
    write_frame_bits();
    frames_written_++;
}

void Encoder::feed(const float* interleaved, int frames) {
    if (flushed_)
        throw std::runtime_error("encoder already flushed");
    const int n = mtab_->size;
    pcm_pending_.insert(pcm_pending_.end(), interleaved, interleaved + frames * channels_);

    auto take_frame = [&](std::vector<float>& frame) {
        frame.assign(static_cast<size_t>(n) * channels_, 0.0f);
        if (lead_left_ > 0) {
            lead_left_--;
            return true;
        }
        const int need = n * channels_;
        if (static_cast<int>(pcm_pending_.size()) < need)
            return false;
        std::memcpy(frame.data(), pcm_pending_.data(), static_cast<size_t>(need) * sizeof(float));
        pcm_pending_.erase(pcm_pending_.begin(), pcm_pending_.begin() + need);
        return true;
    };

    std::vector<float> frame;
    while (take_frame(frame))
        encode_frame(frame.data(), false);
}

void Encoder::flush() {
    if (flushed_)
        return;
    const int n = mtab_->size;
    const int need = n * channels_;
    if (!pcm_pending_.empty()) {
        pcm_pending_.resize(static_cast<size_t>(need), 0.0f);
        encode_frame(pcm_pending_.data(), true);
        pcm_pending_.clear();
    }
    // One extra hop so the last overlap is encoded.
    std::vector<float> z(static_cast<size_t>(need), 0.0f);
    encode_frame(z.data(), true);
    flushed_ = true;
    // Pad DATA to whole bytes.
    const int bits = frames_written_ * frame_bits_;
    const int bytes = (bits + 7) >> 3;
    data_.resize(static_cast<size_t>(bytes), 0);
}

VqfInfo Encoder::make_info() const {
    return make_vqf_info(channels_, sample_rate_, bitrate_kbps_, cfg_.tags, data_.size(), cfg_.version);
}

std::vector<uint8_t> Encoder::build_file() const {
    return build_vqf_file(make_info(), data_.data(), data_.size());
}

} // namespace twinvq
