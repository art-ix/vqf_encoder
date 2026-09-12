#include "twinvq_window.hpp"
#include "twinvq/vqf_file.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace twinvq {
namespace {
// Transpose the actual overlap/copy operations, including copies overwritten
// by a later subblock. This is an offline analysis oracle, not a streaming
// encoder: it deliberately sees the complete window schedule.
void analyze_window(const WindowLayout& layout, float* current, float* previous, float* half) {
    for (int j = layout.blocks - 1; j >= 0; --j) {
        const auto& b = layout.block[j];
        float* input = half + j * layout.block_size;
        float* prev = j == 0 ? previous : half;
        for (int i = 0; i < layout.block_size - b.overlap / 2; ++i) {
            input[b.overlap / 2 + i] += current[b.output + b.overlap + i];
            current[b.output + b.overlap + i] = 0;
        }
        const float* win = sine_window_cached(b.overlap);
        for (int i = 0; i < b.overlap / 2; ++i) {
            const int r = b.overlap - 1 - i;
            const float lo = current[b.output + i], hi = current[b.output + r];
            prev[b.previous + i] += lo * win[r] + hi * win[i];
            input[b.overlap / 2 - 1 - i] += -lo * win[i] + hi * win[r];
            current[b.output + i] = current[b.output + r] = 0;
        }
    }
}

float roundtrip(const ModeTab& mode, const std::vector<int>& schedule,
                const std::vector<float>& signal, int channels = 2, bool transforms = true,
                const std::vector<int>* synthesis_schedule = nullptr) {
    const int n = mode.size, frames = static_cast<int>(schedule.size());
    std::vector<WindowLayout> layout;
    for (int type : schedule) layout.push_back(window_layout(mode, window_frame_type(type), type));
    std::vector<std::vector<float>> state(frames + 1, std::vector<float>(2 * n));
    std::vector<std::vector<float>> half(frames, std::vector<float>(n));
    for (int f = frames - 1; f >= 0; --f) {
        const int last = f ? layout[f - 1].output_size : 0;
        const int prefix = n - layout[f].output_size;
        for (int i = 0; i < prefix; ++i) state[f][last + i] += signal[f * n + i];
        for (int i = prefix; i < n; ++i) state[f + 1][i - prefix] += signal[f * n + i];
        analyze_window(layout[f], state[f + 1].data(), state[f].data() + last, half[f].data());
    }
    // Compare the bounded production analysis against the independent
    // full-schedule oracle, including transitions and the zero-padded tail.
    for (int f = 0; f < frames; ++f) {
        std::vector<float> pair(2 * n), actual(n);
        std::copy_n(signal.data() + f * n, n, pair.data());
        if (f + 1 < frames) std::copy_n(signal.data() + (f + 1) * n, n, pair.data() + n);
        const auto next = f + 1 < frames ? layout[f + 1] : window_layout(mode, FrameType::Long, 0);
        analyze_window_pair(layout[f], next, pair.data(), actual.data());
        for (int i = 0; i < n; ++i)
            if (std::fabs(actual[i] - half[f][i]) > 2.0e-6f) return INFINITY;
    }
    if (synthesis_schedule) {
        layout.clear();
        for (int type : *synthesis_schedule) layout.push_back(window_layout(mode, window_frame_type(type), type));
    }
    const bool long_control = !synthesis_schedule &&
        std::all_of(schedule.begin(), schedule.end(), [](int type) { return type == 0; });
    std::vector<float> spectrum(n), time(2 * n), previous(2 * n), current(2 * n), reference(n);
    float error = 0;
    int last = 0;
    for (int f = 0; f < frames; ++f) {
        const int b = layout[f].block_size;
        for (int j = 0; transforms && j < layout[f].blocks; ++j) {
            std::fill(time.begin(), time.end(), 0.0f);
            // The middle N IMDCT samples form an orthogonal N x N transform.
            std::copy_n(half[f].data() + j * b, b, time.data() + b / 2);
            const float inverse_scale = -std::sqrt((channels == 1 ? 2.0f : 1.0f) / b) / 32768.0f;
            mdct_forward(spectrum.data() + j * b, time.data(), b, 2.0f / (b * inverse_scale));
            if (long_control) {
                for (int i = 0; i < 2 * n; ++i) {
                    const int pos = f * n + i;
                    time[i] = pos < static_cast<int>(signal.size()) ? signal[pos] : 0.0f;
                    time[i] *= std::sin((i + 0.5f) * (3.14159265358979323846f / (2 * n)));
                }
                mdct_forward(reference.data(), time.data(), n, 2.0f / (n * inverse_scale));
                for (int k = 0; k < n; ++k)
                    error = std::max(error, std::fabs((spectrum[k] - reference[k]) * inverse_scale));
            }
            imdct_half(half[f].data() + j * b, spectrum.data() + j * b, b, inverse_scale);
        }
        synthesize_window(layout[f], half[f].data(), previous.data() + last, current.data());
        const int prefix = n - layout[f].output_size;
        for (int i = 0; i < n; ++i) {
            const float actual = i < prefix ? previous[last + i] : current[i - prefix];
            if (!std::isfinite(actual)) return INFINITY;
            error = std::max(error, std::fabs(actual - signal[f * n + i]));
        }
        last = layout[f].output_size;
        previous.swap(current);
        std::fill(current.begin(), current.end(), 0.0f);
    }
    return error;
}
} // namespace

bool window_transition_self_test(float* max_abs_err) {
    float worst = 0;
    // All nine window IDs, both directions for every pair of block lengths,
    // repeated short/medium frames and a single-frame transient excursion.
    const std::vector<std::vector<int>> schedules = {
        {0, 0, 0, 0, 0},
        {0, 0, 2, 2, 3, 0, 0},
        {0, 0, 8, 8, 5, 0, 0},
        {0, 1, 2, 4, 7, 5, 6, 0},
        {0, 0, 8, 2, 3, 0, 0},
        {0, 0, 2, 3, 0, 0},
        {0, 2, 3, 2, 3, 0, 2, 2, 3, 0},
    };
    int mode_count = 0;
    const auto* modes = legal_modes(mode_count);
    for (int m = 0; m < mode_count; ++m) {
        const auto& mode = *select_mode(modes[m].sample_rate, modes[m].kbps_per_channel, 1);
        const int n = mode.size;
        for (const auto& schedule : schedules) {
            std::vector<float> signal(schedule.size() * n);
            for (int trial = 0; trial < 5; ++trial) {
                uint32_t rng = 1;
                for (int i = n; i < static_cast<int>(signal.size()) - n; ++i) {
                    rng = rng * 1664525u + 1013904223u;
                    signal[i] = trial == 0 ? static_cast<float>(rng >> 8) / 8388608.0f - 1.0f
                              : trial == 1 ? 0.6f * std::sin(0.173f * i) + 0.2f * std::cos(1.713f * i)
                              : trial == 2 ? 0.75f
                              : trial == 3 ? (i == n || i == static_cast<int>(signal.size()) - n - 1 ? 1.0f : 0.0f)
                              : (i % n == n / 2 ? 1.0f : 0.0f);
                }
                for (int channels : {1, 2})
                    worst = std::max(worst, roundtrip(mode, schedule, signal, channels));
            }
            // Sweep impulses across every frame offset using just the window
            // operator. FFT accuracy is checked above and by mdct_self_test;
            // keeping it out of the exhaustive sweep makes this CI-friendly.
            // Cover each distinct geometry once, including the 80/96 layout.
            if (modes[m].sample_rate == 8000 || modes[m].sample_rate == 16000 ||
                (modes[m].sample_rate == 22050 && modes[m].kbps_per_channel == 32) ||
                (modes[m].sample_rate == 44100 && modes[m].kbps_per_channel == 40)) {
                std::fill(signal.begin(), signal.end(), 0.0f);
                for (int offset = 0; offset < n; ++offset) {
                    for (int f = 1; f < static_cast<int>(schedule.size()) - 1; ++f)
                        signal[f * n + offset] = (f & 1) ? 0.75f : -0.5f;
                    worst = std::max(worst, roundtrip(mode, schedule, signal, 2, false));
                    for (int f = 1; f < static_cast<int>(schedule.size()) - 1; ++f)
                        signal[f * n + offset] = 0;
                }
            }
        }
    }
    // Analysis and synthesis must agree on the exit overlap. Deliberately
    // change that window only on synthesis to ensure the oracle catches it.
    const auto& mode = *select_mode(44100, 80, 2);
    const std::vector<int> analysis = {0, 0, 2, 3, 0};
    const std::vector<int> mismatch = {0, 0, 2, 0, 0};
    std::vector<float> signal(analysis.size() * mode.size);
    for (int i = mode.size; i < static_cast<int>(signal.size()) - mode.size; ++i)
        signal[i] = std::sin(0.713f * i);
    const bool rejects_invalid = roundtrip(mode, analysis, signal, 2, true, &mismatch) > 0.01f;
    if (max_abs_err) *max_abs_err = worst;
    return rejects_invalid && worst < 5.0e-6f;
}
} // namespace twinvq
