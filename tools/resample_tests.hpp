#pragma once

#include <cstdlib>
#include <utility>

namespace {
int test_resample() {
    constexpr double pi = 3.14159265358979323846;
    auto require = [](bool ok, const char* reason) {
        if (!ok) throw std::runtime_error(reason);
    };
    const std::vector<float> exact = {0.1f, -0.7f, 1.2f, 0.0f};
    require(audio::resample(exact, 2, 44100, 44100) == exact, "native rate changed");
    require(audio::resample({}, 2, 48000, 44100).empty(), "empty input changed");
    require(audio::resample({0.25f}, 1, 8000, 16000) ==
            std::vector<float>({0.25f, 0.25f}), "single sample boundary failed");
    for (const auto& rates : {std::pair<int,int>{48000,44100}, {88200,44100},
                            {96000,44100}, {32000,44100}, {24000,22050},
                            {12000,11025}, {6000,8000}}) {
        const int in_rate = rates.first, out_rate = rates.second;
        const int count = in_rate / 5 + 1;
        std::vector<float> input(count * 2, 0.25f);
        auto output = audio::resample(input, 2, in_rate, out_rate);
        require(output.size() == static_cast<size_t>(std::llround(
            static_cast<double>(count) * out_rate / in_rate)) * 2, "resampled length failed");
        for (float x : output) require(std::fabs(x - 0.25f) < 1e-6f, "DC/boundary gain failed");

        // Analytic sinusoid oracle: test amplitude AND phase, independently of
        // filter coefficients. Right channel stays silent to catch crosstalk.
        double worst_error = 0;
        const double nyquist = 0.5 * std::min(in_rate, out_rate);
        for (double fraction : {0.05, 0.5, 0.9}) {
            const double frequency = fraction * nyquist;
            for (int i = 0; i < count; ++i) {
                input[2*i] = static_cast<float>(0.5 * std::sin(2*pi*frequency*i/in_rate));
                input[2*i+1] = 0;
            }
            output = audio::resample(input, 2, in_rate, out_rate);
            double error = 0, signal = 0;
            for (size_t i = 512; i + 512 < output.size()/2; ++i) {
                const double expected = 0.5 * std::sin(2*pi*frequency*i/out_rate);
                error += std::pow(output[2*i] - expected, 2);
                signal += expected * expected;
                require(output[2*i+1] == 0, "resampler channel leakage");
            }
            worst_error = std::max(worst_error, std::sqrt(error / signal));
        }
        require(worst_error < 0.001, "passband amplitude/phase regression");

        // Out-of-band tones must disappear, including the old 30 kHz ->
        // 14.1 kHz alias at 88.2 -> 44.1 kHz. Test multiple stopband points.
        double worst_alias = 0;
        if (in_rate > out_rate) {
            std::vector<double> frequencies;
            for (double position : {0.0, 0.25, 0.75})
                frequencies.push_back(out_rate*0.5 + position*(in_rate-out_rate)*0.5);
            if (in_rate == 88200) frequencies.push_back(30000);
            for (double frequency : frequencies) {
                for (int i = 0; i < count; ++i)
                    input[2*i] = static_cast<float>(0.5 * std::sin(2*pi*frequency*i/in_rate));
                output = audio::resample(input, 2, in_rate, out_rate);
                double energy = 0;
                size_t measured = 0;
                for (size_t i = 512; i + 512 < output.size()/2; ++i) {
                    energy += static_cast<double>(output[2*i])*output[2*i];
                    ++measured;
                }
                worst_alias = std::max(worst_alias, std::sqrt(energy / measured / 0.125));
            }
            require(worst_alias < 0.0001, "stopband rejection below 80 dB");
        }
        std::fill(input.begin(), input.end(), 0);
        const int impulse = in_rate / 10;
        input[impulse*2] = 1;
        output = audio::resample(input, 2, in_rate, out_rate);
        size_t peak = 0;
        for (size_t i = 0; i < output.size()/2; ++i)
            if (std::fabs(output[2*i]) > std::fabs(output[2*peak])) peak = i;
        require(std::llabs(static_cast<long long>(peak) - std::llround(
            static_cast<double>(impulse)*out_rate/in_rate)) <= 1, "resampler delay regression");
        std::cout << in_rate << " -> " << out_rate << ": passband relative error="
                  << worst_error << " stopband relative RMS=" << worst_alias << '\n';
    }
    std::cout << "resampler regression tests passed\n";
    return 0;
}
} // namespace
