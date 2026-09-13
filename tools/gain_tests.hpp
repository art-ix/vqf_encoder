#pragma once
#include "twinvq_gain_search.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

inline int test_gain_search() {
    size_t checks = 0;
    auto check = [&](const std::array<float, 32>& values, double target) {
        int index = 0;
        double error = std::numeric_limits<double>::infinity();
        for (int q = 0; q < 32; ++q) {
            const double d = values[q] - target;
            if (d * d < error) { error = d * d; index = q; }
        }
        for (int hint : {0, 7, 16, 31}) {
            const auto got = twinvq::detail::nearest_gain(values.data(), 32, target, hint);
            if (got.index != index || got.error != error)
                throw std::runtime_error("sorted gain search differs from exhaustive oracle");
            ++checks;
        }
    };
    std::mt19937 rng(7813);
    for (int row = 0; row < 256; ++row) {
        std::array<float, 32> values;
        for (int q = 0; q < 32; ++q)
            values[q] = std::ldexp(static_cast<float>(q * q + q + 1), row % 101 - 50);
        for (int q = 0; q < 32; ++q) {
            check(values, values[q]);
            if (q) {
                const double midpoint = (static_cast<double>(values[q - 1]) + values[q]) * 0.5;
                check(values, midpoint);
                check(values, std::nextafter(midpoint, -INFINITY));
                check(values, std::nextafter(midpoint, INFINITY));
            }
        }
        for (int i = 0; i < 256; ++i)
            check(values, std::ldexp(static_cast<double>(rng()) / rng.max(), row % 101 - 45));
        for (double target : {0.0, -1.0, 1e100, -1e100, 1e300, -1e300,
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) check(values, target);
        for (int q = 0; q < 32; ++q) values[q] = static_cast<float>(q / 4);
        for (double target : {-1.0, 0.0, 0.5, 3.0, 6.5, 7.0, 100.0}) check(values, target);
    }
    std::cout << checks << " gain searches match exhaustive indices and errors\n";
    return 0;
}
