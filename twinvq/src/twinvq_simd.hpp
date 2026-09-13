#pragma once
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define TWINVQ_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define TWINVQ_TARGET(x) __declspec(noinline)
#else
#define TWINVQ_TARGET(x) __attribute__((target(x), noinline))
#endif
#endif

#if defined(_MSC_VER)
// The enclosing library uses /fp:fast. These ordered kernels must not inherit
// reassociation or contraction; restore the caller's mode after the header.
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

namespace twinvq::detail {
using VectorError = float (*)(const float*, const float*, const int16_t*, int, int, float);

inline float scalar_error(const float* target, const float* weight,
                          const int16_t* code, int sign, int length, float limit) {
    float error = 0;
    int j = 0;
    for (; j + 4 <= length; j += 4) {
        for (int k = 0; k < 4; ++k) {
            const float d = target[j + k] - sign * code[j + k];
            error += weight[j + k] * d * d;
        }
        if (error >= limit) return error;
    }
    for (; j < length; ++j) {
        const float d = target[j] - sign * code[j];
        error += weight[j] * d * d;
    }
    return error;
}

inline bool has_sse41() {
#if defined(TWINVQ_X86) && defined(_MSC_VER)
    int info[4]; __cpuid(info, 1);
    return (info[2] & (1 << 19)) != 0;
#elif defined(TWINVQ_X86)
    return __builtin_cpu_supports("sse4.1");
#else
    return false;
#endif
}
inline bool has_avx2() {
#if defined(TWINVQ_X86) && defined(_MSC_VER)
    int info[4]; __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    if ((info[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28))) return false;
    if ((_xgetbv(0) & 6) != 6) return false;
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#elif defined(TWINVQ_X86)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

#if defined(TWINVQ_X86)
// SIMD evaluates independent terms; scalar accumulation preserves the old
// rounding order and the pruning check after every four terms. No FMA/reduction.
TWINVQ_TARGET("sse4.1") inline float sse41_error(const float* target, const float* weight,
                                const int16_t* code, int sign, int length, float limit) {
    float error = 0;
    int j = 0;
    for (; j + 4 <= length; j += 4) {
        __m128 values = _mm_cvtepi32_ps(_mm_cvtepi16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(code + j))));
        if (sign < 0) values = _mm_xor_ps(values, _mm_set1_ps(-0.0f));
        const __m128 d = _mm_sub_ps(_mm_loadu_ps(target + j), values);
        float terms[4];
        _mm_storeu_ps(terms, _mm_mul_ps(_mm_mul_ps(_mm_loadu_ps(weight + j), d), d));
        for (float term : terms) error += term;
        if (error >= limit) return error;
    }
    for (; j < length; ++j) {
        const float d = target[j] - sign * code[j];
        error += weight[j] * d * d;
    }
    return error;
}

TWINVQ_TARGET("avx2") inline float avx2_error(const float* target, const float* weight,
                               const int16_t* code, int sign, int length, float limit) {
    float error = 0;
    int j = 0;
    for (; j + 8 <= length; j += 8) {
        __m256 values = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(code + j))));
        if (sign < 0) values = _mm256_xor_ps(values, _mm256_set1_ps(-0.0f));
        const __m256 d = _mm256_sub_ps(_mm256_loadu_ps(target + j), values);
        float terms[8];
        _mm256_storeu_ps(terms, _mm256_mul_ps(_mm256_mul_ps(_mm256_loadu_ps(weight + j), d), d));
        for (int k = 0; k < 4; ++k) error += terms[k];
        if (error >= limit) return error;
        for (int k = 4; k < 8; ++k) error += terms[k];
        if (error >= limit) return error;
    }
    // Preserve the four-bin check even in a partial AVX2 group.
    for (; j < length; ++j) {
        const float d = target[j] - sign * code[j];
        error += weight[j] * d * d;
        if ((j + 1) % 4 == 0 && error >= limit) return error;
    }
    return error;
}
#endif
} // namespace twinvq::detail

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
