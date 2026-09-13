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
    // Compare grouped searches with the original candidate-at-a-time loop.
    // Duplicate entries cover ties; finite limits cover no winner and pruning.
    std::vector<CodebookSearch> searches{scalar_search};
#if defined(TWINVQ_X86)
    if (has_sse41()) searches.push_back(sse41_search);
    if (has_avx2()) searches.push_back(avx2_search);
#endif
    constexpr int stride = 72, count = 12;
    int16_t book[stride * count];
    for (int i = 0; i < stride * count; ++i)
        book[i] = static_cast<int16_t>(static_cast<int>(random() % 65536) - 32768);
    std::copy_n(book, stride, book + stride);
    for (int offset = 0; offset < 4; ++offset) for (int length = 1; length <= 65; ++length)
        for (bool signed_code : {false, true}) {
            const float full = scalar_error(target + offset, weight + offset, book + offset,
                                             1, length, std::numeric_limits<float>::infinity());
            for (float limit : {0.0f, full * 0.1f, full, std::numeric_limits<float>::infinity()}) {
                CodebookMatch reference{limit, -1, 1};
                for (int a = 0; a < count; ++a) for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
                    const int sign = s ? -1 : 1;
                    const float error = scalar_error(target + offset, weight + offset,
                        book + a * stride + offset, sign, length, reference.error);
                    if (error < reference.error) reference = {error, a, sign};
                }
                for (auto search : searches) {
                    const auto actual = search(target + offset, weight + offset, book + offset,
                                                stride, length, count, signed_code, limit);
                    if (actual.index != reference.index || actual.sign != reference.sign ||
                        std::memcmp(&actual.error, &reference.error, sizeof(float)))
                        throw std::runtime_error("Grouped SIMD search changed winner/error");
                }
            }
        }
    // A stable sort of full scalar errors is an independent reference for
    // bounded beam selection, including ties and fewer candidates than slots.
    std::vector<BeamSearch> beams{scalar_beam};
#if defined(TWINVQ_X86)
    if (has_sse41()) beams.push_back(sse41_beam);
    if (has_avx2()) beams.push_back(avx2_beam);
#endif
    float zero_weights[72]{};
    for (const float* weights : {static_cast<const float*>(weight), static_cast<const float*>(zero_weights)})
        for (int offset : {0, 3}) for (int length = 1; length <= 65; ++length)
            for (bool signed_code : {false, true}) for (int entries : {1, count}) {
                std::vector<CodebookMatch> ordered;
                for (int a = 0; a < entries; ++a) for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
                    const int sign = s ? -1 : 1;
                    ordered.push_back({scalar_error(target + offset, weights + offset,
                        book + a * stride + offset, sign, length,
                        std::numeric_limits<float>::infinity()), a, sign});
                }
                std::stable_sort(ordered.begin(), ordered.end(),
                    [](const CodebookMatch& a, const CodebookMatch& b) { return a.error < b.error; });
                for (int size : {4, 8, 16, 32}) for (auto beam : beams) {
                    int indices[34], signs[34];
                    std::fill_n(indices, 34, -99); std::fill_n(signs, 34, -99);
                    beam(target + offset, weights + offset, book + offset, stride,
                         length, entries, signed_code, size, indices + 1, signs + 1);
                    if (indices[0] != -99 || indices[size + 1] != -99 ||
                        signs[0] != -99 || signs[size + 1] != -99)
                        throw std::runtime_error("SIMD beam wrote outside output");
                    for (int slot = 0; slot < size; ++slot) {
                        const bool valid = slot < static_cast<int>(ordered.size()) && ordered[slot].error < 1.0e30f;
                        if (indices[slot + 1] != (valid ? ordered[slot].index : 0) ||
                            signs[slot + 1] != (valid ? ordered[slot].sign : 0))
                            throw std::runtime_error("SIMD beam changed ordered candidates");
                    }
                }
            }
#if defined(TWINVQ_X86)
    if (has_avx2()) {
        int16_t packed_source[64 * stride + 4];
        for (auto& value : packed_source)
            value = static_cast<int16_t>(static_cast<int>(random() % 65536) - 32768);
        packed_source[4] = -32768; packed_source[6] = 32767; packed_source[7] = 0;
        std::copy_n(packed_source, stride, packed_source + stride);
        for (int offset : {0, 3}) {
            const PackedCodebook packed(packed_source + offset, stride, 64);
            for (int length : {1, 3, 4, 7, 8, 9, 17, 65})
                for (int entries : {1, 3, 12, 64}) for (bool signs : {false, true})
                    for (const float* weights : {static_cast<const float*>(weight), static_cast<const float*>(zero_weights)}) {
                        const float full = scalar_error(target + offset, weights + offset,
                            packed_source + offset, 1, length, std::numeric_limits<float>::infinity());
                        for (int size : {4, 8, 16, 32}) {
                            int reference_indices[32], reference_signs[32], indices[34], signs_out[34];
                            std::fill_n(indices, 34, -99); std::fill_n(signs_out, 34, -99);
                            scalar_beam(target + offset, weights + offset, packed_source + offset,
                                        stride, length, entries, signs, size, reference_indices, reference_signs);
                            avx2_candidate_beam(target + offset, weights + offset, packed,
                                                length, entries, signs, size, indices + 1, signs_out + 1);
                            if (indices[0] != -99 || indices[size + 1] != -99 ||
                                signs_out[0] != -99 || signs_out[size + 1] != -99 ||
                                !std::equal(indices + 1, indices + 1 + size, reference_indices) ||
                                !std::equal(signs_out + 1, signs_out + 1 + size, reference_signs))
                                throw std::runtime_error("Candidate-lane AVX2 changed beam order or bounds");
                        }
                        for (float limit : {0.0f, full * 0.7f, full, std::numeric_limits<float>::infinity()}) {
                            const auto reference = scalar_search(target + offset, weights + offset,
                                packed_source + offset, stride, length, entries, signs, limit);
                            const auto actual = avx2_candidates(target + offset, weights + offset,
                                packed, length, entries, signs, limit);
                            if (actual.index != reference.index || actual.sign != reference.sign ||
                                std::memcmp(&actual.error, &reference.error, sizeof(float)))
                                throw std::runtime_error("Candidate-lane AVX2 changed winner/error");
                        }
                    }
        }
    }
#endif
    std::cout << "SIMD kernels passed: scalar=1 sse41=" << has_sse41() << " avx2=" << has_avx2() << "\n";
    return 0;
}
