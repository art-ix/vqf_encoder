#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <numeric>
#include <vector>

namespace audio {

// Offline, centered windowed-sinc conversion. Output sample zero corresponds
// to input sample zero; endpoint extension avoids fading a constant signal.
inline std::vector<float> resample(const std::vector<float>& input,
                                   int channels, int input_rate, int output_rate) {
    if (channels < 1 || channels > 2 || input_rate <= 0 || output_rate <= 0 ||
        input.size() % channels != 0)
        throw std::invalid_argument("invalid resampler format");
    if (input_rate == output_rate || input.empty()) return input;
    const size_t frames = input.size() / channels;
    const double ratio = static_cast<double>(output_rate) / input_rate;
    const size_t count = static_cast<size_t>(std::llround(frames * ratio));
    std::vector<float> output(count * channels);
    if (!count) return output;

    // Transition band: approximately 90–100% of the lower Nyquist frequency.
    // Scale the support for downsampling to retain stopband attenuation.
    constexpr double pi = 3.14159265358979323846;
    const int phases = std::min(1024, output_rate / std::gcd(input_rate, output_rate));
    const double cutoff = 0.95 * std::min(1.0, ratio);
    const double support = 96.0 / cutoff;
    if (support > 4096)
        throw std::invalid_argument("unsupported resampling ratio");
    const int radius = static_cast<int>(std::ceil(support));
    const int taps = 2 * radius + 1;
    std::vector<double> table(static_cast<size_t>(phases + 1) * taps);
    for (int phase = 0; phase <= phases; ++phase) {
        double sum = 0;
        double* row = table.data() + static_cast<size_t>(phase) * taps;
        for (int k = -radius; k <= radius; ++k) {
            const double x = k - static_cast<double>(phase) / phases;
            double value = 0;
            if (std::fabs(x) < support) {
                const double z = pi * cutoff * x;
                const double sinc = std::fabs(z) < 1e-12 ? 1.0 : std::sin(z) / z;
                const double window = 0.42 + 0.5 * std::cos(pi * x / support)
                                          + 0.08 * std::cos(2 * pi * x / support);
                value = cutoff * sinc * window;
            }
            row[k + radius] = value;
            sum += value;
        }
        for (int k = 0; k < taps; ++k) row[k] /= sum;
    }

    // Integer phase accumulation prevents drift on long recordings.
    int64_t center = 0, remainder = 0;
    for (size_t i = 0; i < count; ++i) {
        const double phase = static_cast<double>(remainder) * phases / output_rate;
        const int p = static_cast<int>(phase);
        const double fraction = phase - p;
        const double* row = table.data() + static_cast<size_t>(p) * taps;
        double sums[2] = {};
        for (int k = -radius; k <= radius; ++k) {
            const size_t index = static_cast<size_t>(std::clamp<int64_t>(
                center + k, 0, static_cast<int64_t>(frames) - 1));
            const int t = k + radius;
            const double weight = row[t] + fraction * (row[t + taps] - row[t]);
            for (int ch = 0; ch < channels; ++ch)
                sums[ch] += input[index * channels + ch] * weight;
        }
        for (int ch = 0; ch < channels; ++ch)
            output[i * channels + ch] = static_cast<float>(sums[ch]);
        remainder += input_rate;
        center += remainder / output_rate;
        remainder %= output_rate;
    }
    return output;
}

} // namespace audio
