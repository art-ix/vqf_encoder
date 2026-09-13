#pragma once

namespace twinvq {

// Experimental simultaneous-masking weights. Input is unquantized mono or
// planar M/S MDCT, with n bins per channel. Output has the same dimensions.
// Relative thresholds only: no assumed playback SPL or temporal masking.
void psychoacoustic_weights(const float* spectrum, int n, int channels,
                             int sample_rate, float* weights);
// Multiply existing weights using broadband activity; this is not speech detection.
void sibilant_weights(const float* spectrum, int n, int channels,
                      int sample_rate, float* weights);
// Multiply existing weights in tonal treble, with extra protection in spectral gaps.
// The analysis is per subblock, relative to input energy and shared by M/S.
void tonal_noise_weights(const float* spectrum, int n, int channels,
                         int sample_rate, float* weights);
bool psychoacoustic_self_test();
bool sibilant_self_test();
bool tonal_noise_self_test();

} // namespace twinvq
