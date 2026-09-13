#include "twinvq_psychoacoustic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace twinvq {
namespace {
constexpr int kBands = 26;
struct Band {
    double energy = 0;
    double logs = 0;
    double center = 0;
    int count = 0;
};

double bark(double hz) {
    return 13.0 * std::atan(0.00076 * hz) +
           3.5 * std::atan((hz / 7500.0) * (hz / 7500.0));
}

std::array<double, kBands> thresholds(const std::vector<double>& power,
    const std::vector<int>& band_id, const std::vector<double>& position, double floor) {
    std::array<Band, kBands> bands{};
    for (size_t i = 0; i < power.size(); ++i) {
        auto& b = bands[band_id[i]];
        b.energy += power[i];
        b.logs += std::log(std::max(power[i], floor));
        b.center += position[i];
        ++b.count;
    }
    std::array<double, kBands> masker{}, result{};
    for (int b = 0; b < kBands; ++b) {
        if (!bands[b].count) continue;
        const double mean = bands[b].energy / bands[b].count;
        const double flatness = std::clamp(std::exp(bands[b].logs / bands[b].count) /
                                           std::max(mean, floor), 1.0e-12, 1.0);
        // Initial heuristic: flat noise allows a larger error than a tone.
        // These constants are tunable, not measured hearing thresholds.
        const double tonal = std::clamp(-std::log10(flatness) / 3.0, 0.0, 1.0);
        masker[b] = mean * std::pow(10.0, -(6.0 + 12.0 * tonal) / 10.0);
        bands[b].center /= bands[b].count;
    }
    for (int b = 0; b < kBands; ++b) {
        result[b] = floor;
        if (!bands[b].count) continue;
        for (int m = 0; m < kBands; ++m) {
            if (!bands[m].count) continue;
            const double distance = bands[b].center - bands[m].center;
            // Conservative maximum, not a sum of many overlapping maskers.
            const double attenuation = std::fabs(distance) * (distance >= 0 ? 12.0 : 27.0);
            result[b] = std::max(result[b], masker[m] * std::pow(10.0, -attenuation / 10.0));
        }
    }
    return result;
}
} // namespace

void psychoacoustic_weights(const float* spectrum, int n, int channels,
                             int sample_rate, float* weights) {
    std::fill_n(weights, n * channels, 1.0f);
    std::vector<double> left(n), right(n), position(n);
    std::vector<int> band_id(n);
    double energy = 0;
    for (int i = 0; i < n; ++i) {
        const double mid = spectrum[i];
        const double side = channels == 2 ? spectrum[n + i] : 0;
        left[i] = (mid + side) * (mid + side);
        right[i] = (mid - side) * (mid - side);
        energy += left[i] + right[i];
        position[i] = bark((i + 0.5) * sample_rate / (2.0 * n));
        band_id[i] = std::min(kBands - 1, static_cast<int>(position[i]));
    }
    // Silence has no reliable relative masking estimate. Double precision
    // keeps squared float MDCT coefficients and gain changes well behaved.
    if (!(energy > 1.0e-60) || !std::isfinite(energy)) return;
    const double floor = energy / (2 * n) * 1.0e-12;
    const auto l = thresholds(left, band_id, position, floor);
    const auto r = channels == 2 ? thresholds(right, band_id, position, floor) : l;
    double reference = 0;
    for (int i = 0; i < n; ++i) reference += std::min(l[band_id[i]], r[band_id[i]]);
    reference /= n;
    for (int i = 0; i < n; ++i) {
        // Use the stricter ear and identical M/S weights. This avoids assuming
        // interaural masking or dropping cross terms from unequal L/R weights.
        // Fourth-root compression limits the tradeoff against ordinary squared
        // error. The largest relative weight ratio is four, rather than sixteen.
        const double threshold = std::min(l[band_id[i]], r[band_id[i]]);
        const float w = static_cast<float>(std::clamp(std::sqrt(std::sqrt(reference / threshold)), 0.5, 2.0));
        weights[i] = w;
        if (channels == 2) weights[n + i] = w;
    }
}

// Relative broadband detector, not a speech/phoneme recognizer. Equal M/S
// weights avoid inventing interaural masking. The input weights may already
// include the optional simultaneous-masking model.
void sibilant_weights(const float* spectrum, int n, int channels,
                      int sample_rate, float* weights) {
    if (sample_rate < 16000) return;
    const double upper = std::min(9000.0, sample_rate * 0.45);
    double total = 0, high = 0, logs = 0;
    int count = 0;
    std::vector<double> power(n);
    for (int i = 0; i < n; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const double x = spectrum[ch * n + i];
            power[i] += x * x;
        }
        total += power[i];
        const double hz = (i + 0.5) * sample_rate / (2.0 * n);
        if (hz >= 2500 && hz <= upper) { high += power[i]; ++count; }
    }
    if (!(total > 1.0e-60) || !std::isfinite(total) || !count || high <= 0) return;
    const double floor = total / n * 1.0e-12;
    for (int i = 0; i < n; ++i) {
        const double hz = (i + 0.5) * sample_rate / (2.0 * n);
        if (hz >= 2500 && hz <= upper) logs += std::log(std::max(power[i], floor));
    }
    const double flatness = std::exp(logs / count) / (high / count);
    const double activity = std::clamp((high / total - 0.02) / 0.18, 0.0, 1.0) *
                            std::clamp((flatness - 0.04) / 0.20, 0.0, 1.0);
    for (int i = 0; i < n; ++i) {
        const double hz = (i + 0.5) * sample_rate / (2.0 * n);
        const double taper = std::clamp((hz - 2000.0) / 1000.0, 0.0, 1.0) *
                             std::clamp((upper + 1000.0 - hz) / 2000.0, 0.0, 1.0);
        const double voice_band = std::clamp((hz - 150.0) / 350.0, 0.0, 1.0) *
                                  std::clamp((3500.0 - hz) / 1000.0, 0.0, 1.0);
        // Keep the overlap maximum unchanged while moving protection toward
        // the broadband fricative band for a real-material A/B experiment.
        const float emphasis = static_cast<float>(1.0 + activity * (2.25 * taper + 0.50 * voice_band));
        for (int ch = 0; ch < channels; ++ch) weights[ch * n + i] *= emphasis;
    }
}

bool sibilant_self_test() {
    constexpr int n = 2048;
    std::vector<float> spectrum(2 * n), weights(2 * n, 1), other(2 * n);
    sibilant_weights(spectrum.data(), n, 2, 44100, weights.data());
    for (float w : weights) if (w != 1) return false;
    // Isolated tones, including treble, must not trigger broadband protection.
    for (int bin : {40, 500}) {
        spectrum[bin] = 1;
        sibilant_weights(spectrum.data(), n, 2, 44100, weights.data());
        for (float w : weights) if (w != 1) return false;
        spectrum[bin] = 0;
    }
    for (int i = 0; i < n; ++i) {
        const double hz = (i + 0.5) * 44100 / (2.0 * n);
        spectrum[i] = hz >= 2500 && hz <= 9000 ? 1 : 0;
        spectrum[n + i] = spectrum[i] * 0.5f;
    }
    sibilant_weights(spectrum.data(), n, 2, 44100, weights.data());
    if (weights[400] <= 2 || weights[100] <= 1 || weights[0] != 1) return false;
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(weights[i]) || weights[i] < 1 || weights[i] > 3.75f ||
            weights[i] != weights[n + i]) return false;
    for (float factor : {-1.0f, 1.0f / 65536.0f, 65536.0f}) {
        auto scaled = spectrum;
        for (float& x : scaled) x *= factor;
        std::fill(other.begin(), other.end(), 1);
        sibilant_weights(scaled.data(), n, 2, 44100, other.data());
        for (int i = 0; i < 2 * n; ++i)
            if (std::fabs(other[i] - weights[i]) > 1e-5f) return false;
    }
    // An ear swap negates S; existing weights must be multiplied, not replaced.
    for (int i = n; i < 2 * n; ++i) spectrum[i] *= -1;
    std::fill(other.begin(), other.end(), 0.5f);
    sibilant_weights(spectrum.data(), n, 2, 44100, other.data());
    for (int i = 0; i < 2 * n; ++i)
        if (other[i] != weights[i] * 0.5f) return false;
    std::fill(other.begin(), other.end(), 1);
    sibilant_weights(spectrum.data(), n, 1, 8000, other.data());
    for (float w : other) if (w != 1) return false;
    return true;
}

bool psychoacoustic_self_test() {
    const int n = 2048;
    std::vector<float> spectrum(2 * n), weights(2 * n), other(2 * n);
    psychoacoustic_weights(spectrum.data(), n, 2, 44100, weights.data());
    for (float w : weights) if (w != 1.0f) return false;
    for (int i = 0; i < n; ++i) {
        spectrum[i] = static_cast<float>(std::sin(i * 0.71) * std::exp(-i / 400.0));
        spectrum[n + i] = static_cast<float>(0.2 * std::cos(i * 0.19));
    }
    psychoacoustic_weights(spectrum.data(), n, 2, 44100, weights.data());
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(weights[i]) || weights[i] < 0.5f || weights[i] > 2.0f ||
            weights[i] != weights[n + i]) return false;
    // Overall gain and polarity must not change a relative masking model.
    for (float factor : {-1.0f, 1.0f / 65536.0f, 65536.0f}) {
        auto scaled = spectrum;
        for (float& x : scaled) x *= factor;
        psychoacoustic_weights(scaled.data(), n, 2, 44100, other.data());
        for (int i = 0; i < 2 * n; ++i)
            if (std::fabs(other[i] - weights[i]) > 1.0e-5f) return false;
    }
    // Swapping the ears is a side polarity change.
    for (int i = n; i < 2 * n; ++i) spectrum[i] *= -1;
    psychoacoustic_weights(spectrum.data(), n, 2, 44100, other.data());
    if (weights != other) return false;
    // Equal band energy: a concentrated tone must mask less than flat noise.
    std::vector<double> tonal(32, 0), noise(32, 1), position(32, 5.5);
    std::vector<int> id(32, 5);
    tonal[16] = 32;
    const auto tone_mask = thresholds(tonal, id, position, 1e-12);
    const auto noise_mask = thresholds(noise, id, position, 1e-12);
    if (!(tone_mask[5] < noise_mask[5] * 0.1)) return false;
    // A masker may affect nearby bands, with a smaller effect farther away.
    tonal.assign(3, 0); tonal[0] = 1;
    id = {5, 6, 8}; position = {5.5, 6.5, 8.5};
    const auto spread = thresholds(tonal, id, position, 1e-12);
    return spread[5] > spread[6] && spread[6] > spread[8] && spread[8] > 1e-12;
}
} // namespace twinvq
