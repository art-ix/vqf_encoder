#include "twinvq_mdct.hpp"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace twinvq {

constexpr float kPi = 3.14159265358979323846f;
constexpr double kPiD = 3.14159265358979323846;

namespace {

int log2_pow2(int n) {
    int b = 0;
    while ((1 << b) < n)
        b++;
    return b;
}

// Direct IMDCT matching TwinVQ:
//   y[i] = scale * sum_k X[k] cos(pi/N * (N + i + 1/2) * (k + 1/2))
// i.e. the middle N samples of a 2N-point IMDCT. Correctness oracle.
void imdct_half_direct(float* output, const float* input, int ncoeffs, float scale) {
    const int N = ncoeffs;
    const double pi_over_n = kPiD / static_cast<double>(N);
    for (int i = 0; i < N; i++) {
        const double a = (static_cast<double>(N) + i + 0.5) * pi_over_n;
        double c0 = std::cos(a * 0.5);
        double c1 = std::cos(a * 1.5);
        const double w = 2.0 * std::cos(a);
        double sum = static_cast<double>(input[0]) * c0 + static_cast<double>(input[1]) * c1;
        for (int k = 2; k < N; k++) {
            const double c2 = w * c1 - c0;
            sum += static_cast<double>(input[k]) * c2;
            c0 = c1;
            c1 = c2;
        }
        output[i] = static_cast<float>(sum * static_cast<double>(scale));
    }
}

struct MdctPlan {
    int ncoeffs = 0;
    int fft_n = 0;
    int bits = 0;
    std::vector<float> pre_re;
    std::vector<float> pre_im;
    std::vector<float> post_c;
    std::vector<float> post_s;
    std::vector<float> wr;
    std::vector<float> wi;
    std::vector<int> rev;
    std::vector<float> forward_pre_c, forward_pre_s;
    std::vector<float> forward_post_c, forward_post_s;
};

// Fast path: y[i] = Re{ j * e^{j π (i+1/2)/(2N)} * IFFT_{2N}(S)[i] }
// with S[k] = X[k] * (-1)^k * e^{j π k / (2N)} for k = 0..N-1, else 0.
// Equivalent: y[i] = -sin(α) Re(IFFT[i]) - cos(α) Im(IFFT[i]), α = π(i+1/2)/(2N).
MdctPlan make_plan(int ncoeffs) {
    MdctPlan p;
    p.ncoeffs = ncoeffs;
    p.fft_n = ncoeffs * 2;
    p.bits = log2_pow2(p.fft_n);
    const int N = ncoeffs;
    const int M = p.fft_n;

    p.forward_pre_c.resize(M);
    p.forward_pre_s.resize(M);
    for (int i = 0; i < M; ++i) {
        const double a = kPiD * i / M;
        p.forward_pre_c[i] = static_cast<float>(std::cos(a));
        p.forward_pre_s[i] = static_cast<float>(std::sin(a));
    }
    p.forward_post_c.resize(N);
    p.forward_post_s.resize(N);
    for (int k = 0; k < N; ++k) {
        const double a = kPiD / N * (0.5 + 0.5 * N) * (k + 0.5);
        p.forward_post_c[k] = static_cast<float>(std::cos(a));
        p.forward_post_s[k] = static_cast<float>(std::sin(a));
    }

    p.pre_re.resize(static_cast<size_t>(N));
    p.pre_im.resize(static_cast<size_t>(N));
    for (int k = 0; k < N; k++) {
        const double theta = kPiD * static_cast<double>(k) / static_cast<double>(M);
        const double sign = (k & 1) ? -1.0 : 1.0;
        p.pre_re[static_cast<size_t>(k)] = static_cast<float>(sign * std::cos(theta));
        p.pre_im[static_cast<size_t>(k)] = static_cast<float>(sign * std::sin(theta));
    }

    p.post_c.resize(static_cast<size_t>(N));
    p.post_s.resize(static_cast<size_t>(N));
    for (int i = 0; i < N; i++) {
        const double alpha = kPiD * (static_cast<double>(i) + 0.5) / static_cast<double>(M);
        p.post_c[static_cast<size_t>(i)] = static_cast<float>(std::cos(alpha));
        p.post_s[static_cast<size_t>(i)] = static_cast<float>(std::sin(alpha));
    }

    p.rev.resize(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) {
        unsigned x = static_cast<unsigned>(i);
        unsigned y = 0;
        for (int b = 0; b < p.bits; b++) {
            y = (y << 1) | (x & 1u);
            x >>= 1;
        }
        p.rev[static_cast<size_t>(i)] = static_cast<int>(y);
    }

    const int ntw = M / 2;
    p.wr.resize(static_cast<size_t>(ntw));
    p.wi.resize(static_cast<size_t>(ntw));
    for (int i = 0; i < ntw; i++) {
        const double a = 2.0 * kPiD * static_cast<double>(i) / static_cast<double>(M);
        p.wr[static_cast<size_t>(i)] = static_cast<float>(std::cos(a));
        p.wi[static_cast<size_t>(i)] = static_cast<float>(std::sin(a));
    }
    return p;
}

const MdctPlan& plan_for(int ncoeffs) {
    static MdctPlan plans[16];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int n = 32; n <= 2048; n <<= 1)
            plans[log2_pow2(n)] = make_plan(n);
    });
    return plans[log2_pow2(ncoeffs)];
}

// In-place radix-2 DIT inverse FFT. `data` is n interleaved complex samples
// already in bit-reversed order; output is natural order. No 1/n scale.
void ifft_dit(float* data, int n, const float* wr, const float* wi) {
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            int tw = 0;
            for (int j = 0; j < half; j++, tw += step) {
                float* a = data + ((i + j) << 1);
                float* b = data + ((i + j + half) << 1);
                const float tr = wr[tw] * b[0] - wi[tw] * b[1];
                const float ti = wr[tw] * b[1] + wi[tw] * b[0];
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
        }
    }
}

void imdct_half_fft(float* output, const float* input, int ncoeffs, float scale) {
    const MdctPlan& p = plan_for(ncoeffs);
    const int N = ncoeffs;
    const int M = p.fft_n;

    thread_local std::vector<float> scratch;
    const size_t need = static_cast<size_t>(M) * 2;
    if (scratch.size() < need)
        scratch.assign(need, 0.0f);
    else
        std::memset(scratch.data(), 0, need * sizeof(float));

    const float* pre_re = p.pre_re.data();
    const float* pre_im = p.pre_im.data();
    const int* rev = p.rev.data();
    for (int k = 0; k < N; k++) {
        const int j = rev[k] << 1;
        const float x = input[k];
        scratch[static_cast<size_t>(j)] = x * pre_re[k];
        scratch[static_cast<size_t>(j) + 1] = x * pre_im[k];
    }

    ifft_dit(scratch.data(), M, p.wr.data(), p.wi.data());

    const float* post_c = p.post_c.data();
    const float* post_s = p.post_s.data();
    for (int i = 0; i < N; i++) {
        const float re = scratch[static_cast<size_t>(i) << 1];
        const float im = scratch[(static_cast<size_t>(i) << 1) + 1];
        output[i] = scale * (-post_s[i] * re - post_c[i] * im);
    }
}

struct SineCache {
    std::vector<float> tabs[16];
    SineCache() {
        for (int n = 32; n <= 2048; n <<= 1) {
            tabs[log2_pow2(n)].resize(static_cast<size_t>(n));
            sine_window(tabs[log2_pow2(n)].data(), n);
        }
    }
};

const float* cached_sine(int n) {
    static SineCache cache;
    return cache.tabs[log2_pow2(n)].data();
}

void mdct_forward_direct(float* output, const float* input_2n, int ncoeffs, float scale) {
    const int N = ncoeffs;
    const double pi_over_n = kPiD / static_cast<double>(N);
    for (int k = 0; k < N; k++) {
        double sum = 0.0;
        for (int n = 0; n < 2 * N; n++) {
            const double a = (static_cast<double>(n) + 0.5 + 0.5 * static_cast<double>(N)) *
                             (static_cast<double>(k) + 0.5) * pi_over_n;
            sum += static_cast<double>(input_2n[n]) * std::cos(a);
        }
        output[k] = static_cast<float>(sum * static_cast<double>(scale));
    }
}

void mdct_forward_fft(float* output, const float* input_2n, int ncoeffs, float scale) {
    const MdctPlan& p = plan_for(ncoeffs);
    const int N = ncoeffs;
    const int M = p.fft_n;

    thread_local std::vector<float> scratch;
    const size_t need = static_cast<size_t>(M) * 2;
    if (scratch.size() < need)
        scratch.resize(need);

    const int* rev = p.rev.data();
    for (int n = 0; n < M; n++) {
        const float xr = input_2n[n] * p.forward_pre_c[n];
        const float xi = input_2n[n] * p.forward_pre_s[n];
        const int j = rev[n] << 1;
        scratch[static_cast<size_t>(j)] = xr;
        scratch[static_cast<size_t>(j) + 1] = xi;
    }

    // Positive-exponent FFT plus the MDCT half-bin and N/2 time shifts.
    ifft_dit(scratch.data(), M, p.wr.data(), p.wi.data());

    for (int k = 0; k < N; k++) {
        const float re = scratch[static_cast<size_t>(k) << 1];
        const float im = scratch[(static_cast<size_t>(k) << 1) + 1];
        const float c = p.forward_post_c[k];
        const float s = p.forward_post_s[k];
        output[k] = scale * (c * re - s * im);
    }
}

} // namespace

void imdct_half(float* output, const float* input, int ncoeffs, float scale) {
    if (ncoeffs >= 32 && ncoeffs <= 2048 && (ncoeffs & (ncoeffs - 1)) == 0)
        imdct_half_fft(output, input, ncoeffs, scale);
    else
        imdct_half_direct(output, input, ncoeffs, scale);
}

void mdct_forward(float* output, const float* input_2n, int ncoeffs, float scale) {
    if (ncoeffs >= 32 && ncoeffs <= 2048 && (ncoeffs & (ncoeffs - 1)) == 0)
        mdct_forward_fft(output, input_2n, ncoeffs, scale);
    else
        mdct_forward_direct(output, input_2n, ncoeffs, scale);
}

bool mdct_roundtrip_test(float* max_abs_err) {
    float worst = 0.0f;
    const int sizes[] = {32, 64, 128, 256, 512, 1024, 2048};
    for (int n : sizes) {
        const int hops = 6;
        std::vector<float> time(static_cast<size_t>(n) * (hops + 2), 0.0f);
        for (int i = 0; i < n * (hops + 1); i++) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            time[static_cast<size_t>(i)] = std::sin(t * 9.0f) * 0.7f + 0.2f * std::cos(t * 21.0f);
        }
        std::vector<float> win2n(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n * 2; i++)
            win2n[static_cast<size_t>(i)] =
                std::sin((static_cast<float>(i) + 0.5f) * (kPi / (2.0f * static_cast<float>(n))));
        const float* winN = sine_window_cached(n);
        const float fwd_scale = 2.0f / static_cast<float>(n);

        std::vector<float> spec(static_cast<size_t>(n));
        std::vector<float> im(static_cast<size_t>(n));
        std::vector<float> prev(static_cast<size_t>(n) * 2, 0.0f);
        std::vector<float> curr(static_cast<size_t>(n) * 2, 0.0f);
        std::vector<float> rec(time.size(), 0.0f);
        std::vector<float> frame(static_cast<size_t>(n) * 2);
        int last_pos = n;
        for (int hop = 0; hop < hops; hop++) {
            for (int i = 0; i < n * 2; i++) {
                const int src = hop * n + i;
                const float s = (src >= 0 && src < static_cast<int>(time.size())) ? time[static_cast<size_t>(src)] : 0.0f;
                frame[static_cast<size_t>(i)] = s * win2n[static_cast<size_t>(i)];
            }
            mdct_forward(spec.data(), frame.data(), n, fwd_scale);
            imdct_half(im.data(), spec.data(), n, 1.0f);
            vector_fmul_window(curr.data(), prev.data() + last_pos, im.data(), winN, n / 2);
            std::memcpy(curr.data() + n, im.data() + n / 2, static_cast<size_t>(n / 2) * sizeof(float));
            last_pos = n;
            for (int i = 0; i < n; i++)
                rec[static_cast<size_t>(hop * n + i)] = curr[static_cast<size_t>(i)];
            prev.swap(curr);
            std::fill(curr.begin(), curr.end(), 0.0f);
        }
        for (int i = n * 2; i < n * hops; i++) {
            const float e = std::fabs(time[static_cast<size_t>(i)] - rec[static_cast<size_t>(i)]);
            if (e > worst)
                worst = e;
        }
    }
    if (max_abs_err)
        *max_abs_err = worst;
    return worst < 5.0e-6f;
}

bool mdct_self_test(float* max_abs_err) {
    float worst = 0;
    for (int n = 32; n <= 2048; n *= 2) {
        std::vector<float> input(2 * n), reference(n), actual(n);
        uint32_t rng = 1;
        for (int trial = 0; trial < 3; ++trial) {
            for (int i = 0; i < 2 * n; ++i) {
                rng = rng * 1664525u + 1013904223u;
                input[i] = trial == 0 ? (i == n / 3 ? 1.0f : 0.0f)
                         : trial == 1 ? static_cast<float>(std::sin(0.17 * i))
                         : static_cast<float>(rng >> 8) / 8388608.0f - 1.0f;
            }
            mdct_forward_direct(reference.data(), input.data(), n, 2.0f / n);
            mdct_forward(actual.data(), input.data(), n, 2.0f / n);
            for (int k = 0; k < n; ++k)
                worst = std::max(worst, std::fabs(reference[k] - actual[k]));
        }
    }
    if (max_abs_err) *max_abs_err = worst;
    return worst < 5.0e-6f;
}

bool imdct_self_test(float* max_abs_err) {
    float worst = 0.0f;
    const int sizes[] = {32, 256, 512, 1024, 2048};
    for (int n : sizes) {
        std::vector<float> in(static_cast<size_t>(n));
        std::vector<float> ref(static_cast<size_t>(n));
        std::vector<float> fast(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            in[static_cast<size_t>(i)] = std::sin(t * 17.0f) * (1.0f - t) + 0.15f * std::cos(t * 41.0f);
        }
        const float scale = -0.00003f;
        imdct_half_direct(ref.data(), in.data(), n, scale);
        imdct_half_fft(fast.data(), in.data(), n, scale);
        for (int i = 0; i < n; i++) {
            const float e = std::fabs(ref[static_cast<size_t>(i)] - fast[static_cast<size_t>(i)]);
            if (e > worst)
                worst = e;
        }
    }
    if (max_abs_err)
        *max_abs_err = worst;
    return worst < 5.0e-6f;
}

void sine_window(float* dst, int n) {
    for (int i = 0; i < n; i++)
        dst[i] = std::sin((static_cast<float>(i) + 0.5f) * (kPi / (2.0f * static_cast<float>(n))));
}

const float* sine_window_cached(int n) {
    return cached_sine(n);
}

void vector_fmul(float* dst, const float* src0, const float* src1, int len) {
    for (int i = 0; i < len; i++)
        dst[i] = src0[i] * src1[i];
}

void vector_fmul_window(float* dst, const float* src0, const float* src1, const float* win, int len) {
    dst += len;
    win += len;
    src0 += len;
    for (int i = -len, j = len - 1; i < 0; i++, j--) {
        const float s0 = src0[i];
        const float s1 = src1[j];
        const float wi = win[i];
        const float wj = win[j];
        dst[i] = s0 * wj - s1 * wi;
        dst[j] = s0 * wi + s1 * wj;
    }
}

} // namespace twinvq
