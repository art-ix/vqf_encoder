#pragma once

namespace twinvq {

// Experimental simultaneous-masking weights. Input is unquantized mono or
// planar M/S MDCT, with n bins per channel. Output has the same dimensions.
// Relative thresholds only: no assumed playback SPL or temporal masking.
void psychoacoustic_weights(const float* spectrum, int n, int channels,
                             int sample_rate, float* weights);
bool psychoacoustic_self_test();

} // namespace twinvq
