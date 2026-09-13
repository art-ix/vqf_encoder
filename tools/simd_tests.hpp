#pragma once
#include "twinvq_simd.hpp"
#include <limits>

int test_simd() {
    using namespace twinvq::detail;
    std::vector<VectorError> kernels{scalar_error};
#if defined(TWINVQ_X86)
    if (has_sse41()) kernels.push_back(sse41_error);
    if (has_avx2()) kernels.push_back(avx2_error);
#endif
    uint32_t state = 173;
    auto random = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    float target[72], weight[72]; int16_t code[72];
    for (int i = 0; i < 72; ++i) {
        target[i] = static_cast<float>(static_cast<int>(random() % 100001) - 50000) / 7.0f;
        weight[i] = i % 5 ? static_cast<float>(random() % 10001) / 10000.0f : 0.0f;
        code[i] = i % 3 == 0 ? -32768 : i % 3 == 1 ? 32767 : static_cast<int16_t>(static_cast<int>(random() % 65536) - 32768);
    }
    for (int offset = 0; offset < 4; ++offset) for (int length = 1; length <= 65; ++length)
        for (int sign : {-1, 1}) {
            const float full = scalar_error(target + offset, weight + offset, code + offset,
                                            sign, length, std::numeric_limits<float>::infinity());
            for (float limit : {0.0f, full * 0.1f, full * 0.7f, full,
                                std::nextafter(full, std::numeric_limits<float>::infinity()),
                                std::numeric_limits<float>::infinity()}) {
                const float reference = scalar_error(target + offset, weight + offset, code + offset, sign, length, limit);
                for (auto kernel : kernels) {
                    const float actual = kernel(target + offset, weight + offset, code + offset, sign, length, limit);
                    if (std::memcmp(&actual, &reference, sizeof(float)))
                        throw std::runtime_error("SIMD changed ordered vector error");
                }
            }
        }
    std::cout << "SIMD kernels passed: scalar=1 sse41=" << has_sse41() << " avx2=" << has_avx2() << "\n";
    return 0;
}
