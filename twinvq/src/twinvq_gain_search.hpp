#pragma once
#include <cmath>
#include <limits>

namespace twinvq::detail {
struct GainMatch { int index; double error; };

// Requires finite, nondecreasing gains, count > 0 and 0 <= hint < count.
// Preserve the exhaustive scan's lowest-index ties,
// including wide equal-error plateaus caused by floating-point rounding.
inline GainMatch nearest_gain(const float* values, int count, double target, int hint) {
    if (std::isnan(target)) return {0, std::numeric_limits<double>::infinity()};
    int best = hint;
    const double delta = values[best] - target;
    double error = delta * delta;
    // Search both directions: hints need not be monotone. Walking right
    // across ties handles distant targets whose squared errors round equal
    // before a later value improves; walking left restores first-index ties.
    while (best + 1 < count) {
        const double d = values[best + 1] - target;
        const double next = d * d;
        if (!(next <= error)) break;
        ++best;
        error = next;
    }
    while (best > 0) {
        const double d = values[best - 1] - target;
        const double previous = d * d;
        if (!(previous <= error)) break;
        --best;
        error = previous;
    }
    return {best, error};
}
} // namespace twinvq::detail
