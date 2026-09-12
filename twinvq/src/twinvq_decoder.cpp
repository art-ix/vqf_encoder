#include "twinvq/twinvq_decoder.hpp"
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

const FrameType kWtypeToFtype[] = {
    FrameType::Long, FrameType::Long, FrameType::Short, FrameType::Long,
    FrameType::Medium, FrameType::Long, FrameType::Long, FrameType::Medium,
    FrameType::Medium};

const uint8_t kWtypeToWsize[] = {0, 0, 2, 2, 2, 1, 0, 1, 1};

float mulawinv(float y, float clip, float mu) {
    y = std::clamp(y / clip, -1.0f, 1.0f);
    const float s = (y < 0) ? -1.0f : 1.0f;
    return clip * s * (std::exp(std::log(1.0f + mu) * std::fabs(y)) - 1.0f) / mu;
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

} // namespace

Decoder::Decoder(const VqfInfo& info) {
    mtab_ = select_mode(info.sample_rate, info.bitrate_kbps, info.channels);
    if (!mtab_)
        throw std::runtime_error("unsupported TwinVQ mode");
    channels_ = info.channels;
    sample_rate_ = info.sample_rate;
    bitrate_bps_ = info.bitrate_kbps * 1000;
    ibps_ = info.bitrate_kbps / info.channels;
    isampf_ = (sample_rate_ == 44100) ? 44 : (sample_rate_ == 22050) ? 22 : (sample_rate_ == 11025) ? 11 : sample_rate_ / 1000;
    frame_bits_ = bitrate_bps_ * mtab_->size / sample_rate_ + 8;

    const int table_size = 2 * mtab_->size * channels_;
    spectrum_.assign(table_size, 0.0f);
    curr_frame_.assign(table_size, 0.0f);
    prev_frame_.assign(table_size, 0.0f);
    tmp_buf_.assign(std::max<int>(mtab_->size, 4096), 0.0f);

    for (int i = 0; i < 3; i++) {
        const int m = 4 * mtab_->size / mtab_->fmode[i].sub;
        const double freq = 2.0 * 3.14159265358979323846 / m;
        cos_tabs_[i].assign(m / 4, 0.0f);
        for (int j = 0; j <= m / 8; j++)
            cos_tabs_[i][j] = static_cast<float>(std::cos((2 * j + 1) * freq));
        for (int j = 1; j < m / 8; j++)
            cos_tabs_[i][m / 4 - j] = cos_tabs_[i][j];
    }

    init_bitstream_params();
    reset();
}

void Decoder::reset() {
    discarded_ = 0;
    last_block_pos_[0] = last_block_pos_[1] = 0;
    std::fill(curr_frame_.begin(), curr_frame_.end(), 0.0f);
    std::fill(prev_frame_.begin(), prev_frame_.end(), 0.0f);
    std::fill(spectrum_.begin(), spectrum_.end(), 0.0f);
    std::memset(lsp_hist_, 0, sizeof(lsp_hist_));
    for (auto& a : bark_hist_)
        for (auto& b : a)
            for (auto& c : b)
                c = 0.1f;
}

void Decoder::init_bitstream_params() {
    const int n_ch = channels_;
    const int total_fr_bits = bitrate_bps_ * mtab_->size / sample_rate_;
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

void Decoder::construct_perm_table(FrameType ftype) {
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

void Decoder::dequant(const uint8_t* cb_bits, float* out, FrameType ftype,
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

void Decoder::dec_gain(FrameType ftype, float* out) {
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

void Decoder::dec_bark_env(const uint8_t* in, int use_hist, int ch, float* out, float gain, FrameType ftype) {
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

void Decoder::decode_lsp(int lpc_idx1, const uint8_t* lpc_idx2, int lpc_hist_idx, float* lsp, float* hist) {
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

void Decoder::eval_lpcenv_or_interp(FrameType ftype, float* out, const float* in, int size, int step, int part) {
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

void Decoder::dec_lpc_spectrum_inv(float* lsp, FrameType ftype, float* lpc) {
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

void Decoder::decode_ppc(int period_coef, int g_coef, const float* shape, float* speech) {
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

void Decoder::read_cb_data(BitReader& br, uint8_t* dst, FrameType ftype) {
    const int fi = static_cast<int>(ftype);
    for (int i = 0; i < n_div_[fi]; i++) {
        const int second = (i >= bits_main_spec_change_[fi]);
        *dst++ = static_cast<uint8_t>(br.get(bits_main_spec_[0][fi][second]));
        *dst++ = static_cast<uint8_t>(br.get(bits_main_spec_[1][fi][second]));
    }
}

bool Decoder::read_bitstream(BitReader& br) {
    br.skip(br.get(8));
    window_type_ = br.get(kWindowTypeBits);
    if (window_type_ > 8)
        return false;
    ftype_ = kWtypeToFtype[window_type_];
    const int sub = mtab_->fmode[static_cast<int>(ftype_)].sub;
    read_cb_data(br, main_coeffs_, ftype_);
    for (int i = 0; i < channels_; i++)
        for (int j = 0; j < sub; j++)
            for (int k = 0; k < mtab_->fmode[static_cast<int>(ftype_)].bark_n_coef; k++)
                bark1_[i][j][k] = static_cast<uint8_t>(br.get(mtab_->fmode[static_cast<int>(ftype_)].bark_n_bit));
    for (int i = 0; i < channels_; i++)
        for (int j = 0; j < sub; j++)
            bark_use_hist_[i][j] = static_cast<uint8_t>(br.get1());
    if (ftype_ == FrameType::Long) {
        for (int i = 0; i < channels_; i++)
            gain_bits_[i] = static_cast<uint8_t>(br.get(kGainBits));
    } else {
        for (int i = 0; i < channels_; i++) {
            gain_bits_[i] = static_cast<uint8_t>(br.get(kGainBits));
            for (int j = 0; j < sub; j++)
                sub_gain_bits_[i * sub + j] = static_cast<uint8_t>(br.get(kSubGainBits));
        }
    }
    for (int i = 0; i < channels_; i++) {
        lpc_hist_idx_[i] = static_cast<uint8_t>(br.get(mtab_->lsp_bit0));
        lpc_idx1_[i] = static_cast<uint8_t>(br.get(mtab_->lsp_bit1));
        for (int j = 0; j < mtab_->lsp_split; j++)
            lpc_idx2_[i][j] = static_cast<uint8_t>(br.get(mtab_->lsp_bit2));
    }
    if (ftype_ == FrameType::Long) {
        read_cb_data(br, ppc_coeffs_, FrameType::Ppc);
        for (int i = 0; i < channels_; i++) {
            p_coef_[i] = br.get(mtab_->ppc_period_bit);
            g_coef_[i] = br.get(mtab_->pgain_bit);
        }
    }
    return true;
}

void Decoder::read_and_decode_spectrum(float* out, FrameType ftype) {
    const int fi = static_cast<int>(ftype);
    const int sub = mtab_->fmode[fi].sub;
    const int block_size = mtab_->size / sub;
    float gain[kChannelsMax * kSubblocksMax];
    float ppc_shape[512];

    dequant(main_coeffs_, out, ftype, mtab_->fmode[fi].cb0, mtab_->fmode[fi].cb1, mtab_->fmode[fi].cb_len_read);
    dec_gain(ftype, gain);

    if (ftype == FrameType::Long) {
        const int cb_len_p = (n_div_[3] + mtab_->ppc_shape_len * channels_ - 1) / n_div_[3];
        dequant(ppc_coeffs_, ppc_shape, FrameType::Ppc, mtab_->ppc_shape_cb,
                mtab_->ppc_shape_cb + cb_len_p * kPpcShapeCbSize, cb_len_p);
    }

    for (int i = 0; i < channels_; i++) {
        float* chunk = out + mtab_->size * i;
        float lsp[kLspCoefsMax];
        for (int j = 0; j < sub; j++) {
            dec_bark_env(bark1_[i][j], bark_use_hist_[i][j], i, tmp_buf_.data(), gain[sub * i + j], ftype);
            vector_fmul(chunk + block_size * j, chunk + block_size * j, tmp_buf_.data(), block_size);
        }
        if (ftype == FrameType::Long)
            decode_ppc(p_coef_[i], g_coef_[i], ppc_shape + i * mtab_->ppc_shape_len, chunk);
        decode_lsp(lpc_idx1_[i], lpc_idx2_[i], lpc_hist_idx_[i], lsp, lsp_hist_[i]);
        dec_lpc_spectrum_inv(lsp, ftype, tmp_buf_.data());
        for (int j = 0; j < sub; j++) {
            vector_fmul(chunk, chunk, tmp_buf_.data(), block_size);
            chunk += block_size;
        }
    }
}

void Decoder::imdct_and_window(FrameType ftype, int wtype, float* in, float* prev, int ch) {
    const int fi = static_cast<int>(ftype);
    const int bsize = mtab_->size / mtab_->fmode[fi].sub;
    const int size = mtab_->size;
    float* buf1 = tmp_buf_.data();
    float* out = curr_frame_.data() + 2 * ch * mtab_->size;
    float* out2 = out;
    const int types_sizes[] = {
        mtab_->size / mtab_->fmode[static_cast<int>(FrameType::Long)].sub,
        mtab_->size / mtab_->fmode[static_cast<int>(FrameType::Medium)].sub,
        mtab_->size / (mtab_->fmode[static_cast<int>(FrameType::Short)].sub * 2),
    };
    int wsize = types_sizes[kWtypeToWsize[wtype]];
    const int first_wsize = wsize;
    float* prev_buf = prev + (size - bsize) / 2;
    const float norm = (channels_ == 1) ? 2.0f : 1.0f;
    const float scale = -std::sqrt(norm / static_cast<float>(bsize)) / 32768.0f;

    for (int j = 0; j < mtab_->fmode[fi].sub; j++) {
        int sub_wtype = (ftype == FrameType::Medium) ? 8 : wtype;
        if (!j && wtype == 4)
            sub_wtype = 4;
        else if (j == mtab_->fmode[fi].sub - 1 && wtype == 7)
            sub_wtype = 7;
        wsize = types_sizes[kWtypeToWsize[sub_wtype]];
        imdct_half(buf1 + bsize * j, in + bsize * j, bsize, scale);
        vector_fmul_window(out2, prev_buf + (bsize - wsize) / 2, buf1 + bsize * j, sine_window_cached(wsize),
                           wsize / 2);
        out2 += wsize;
        std::memcpy(out2, buf1 + bsize * j + wsize / 2, (bsize - wsize / 2) * sizeof(float));
        out2 += (ftype == FrameType::Medium) ? (bsize - wsize) / 2 : bsize - wsize;
        prev_buf = buf1 + bsize * j + bsize / 2;
    }
    last_block_pos_[ch] = (size + first_wsize) / 2;
}

void Decoder::imdct_output(FrameType ftype, int wtype, float* interleaved) {
    float* prev_buf = prev_frame_.data() + last_block_pos_[0];
    for (int i = 0; i < channels_; i++)
        imdct_and_window(ftype, wtype, spectrum_.data() + i * mtab_->size, prev_buf + 2 * i * mtab_->size, i);

    if (!interleaved)
        return;

    const int size2 = last_block_pos_[0];
    const int size1 = mtab_->size - size2;
    // Join the previous/current segments directly in the output, converting
    // mid/side to interleaved left/right for stereo.
    auto emit_segment = [&](const float* mid, float* dst, int count) {
        if (channels_ == 2) {
            const float* side = mid + 2 * mtab_->size;
            for (int i = 0; i < count; i++) {
                dst[i * 2] = mid[i] + side[i];
                dst[i * 2 + 1] = mid[i] - side[i];
            }
        } else {
            std::memcpy(dst, mid, count * sizeof(float));
        }
    };
    emit_segment(prev_buf, interleaved, size1);
    emit_segment(curr_frame_.data(), interleaved + size1 * channels_, size2);
}

int Decoder::decode_packet(const uint8_t* packet, int packet_size, float* out) {
    BitReader br(packet, packet_size);
    if (!read_bitstream(br))
        return 0;
    read_and_decode_spectrum(spectrum_.data(), ftype_);
    const bool emit = discarded_ >= 2;
    imdct_output(ftype_, window_type_, emit ? out : nullptr);
    curr_frame_.swap(prev_frame_);
    if (discarded_ < 2) {
        discarded_++;
        return 0;
    }
    return mtab_->size;
}

} // namespace twinvq
