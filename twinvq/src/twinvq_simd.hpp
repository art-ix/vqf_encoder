#pragma once
#include <cstdint>
#include <algorithm>
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define TWINVQ_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define TWINVQ_TARGET(x) __declspec(noinline)
#define TWINVQ_INLINE_TARGET(x) __forceinline
#else
#define TWINVQ_TARGET(x) __attribute__((target(x), noinline))
#define TWINVQ_INLINE_TARGET(x) __attribute__((target(x), always_inline)) inline
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


// Scan candidates in the original index/sign order. The incumbent is updated
// after each distance, preserving early exits and strict tie handling.
struct CodebookMatch { float error; int index; int sign; };
using CodebookSearch = CodebookMatch (*)(const float*, const float*, const int16_t*,
                                        int, int, int, bool, float);

// Eight consecutive candidate values for each frequency bin. Signed entries
// preserve the original +code/-code order; the final group is zero padded.
struct PackedCodebook {
    const int16_t* source;
    int stride, count;
    std::vector<float> positive, signed_values;
    PackedCodebook(const int16_t* code, int width, int entries)
        : source(code), stride(width), count(entries),
          positive(((entries + 7) / 8) * 8 * width),
          signed_values(((entries * 2 + 7) / 8) * 8 * width) {
        for (int a = 0; a < entries; ++a) for (int j = 0; j < width; ++j) {
            const float v = code[a * width + j];
            positive[(a / 8) * width * 8 + j * 8 + a % 8] = v;
            for (int sign = 0; sign < 2; ++sign) {
                const int k = 2 * a + sign;
                signed_values[(k / 8) * width * 8 + j * 8 + k % 8] = sign ? -v : v;
            }
        }
    }
};

// The caller provides beam_size entries (up to 32) for indices and signs.
using BeamSearch = void (*)(const float*, const float*, const int16_t*,
                             int, int, int, bool, int, int*, int*);

inline CodebookMatch scalar_search(const float* target, const float* weight,
        const int16_t* code, int stride, int length, int count, bool signed_code, float limit) {
    CodebookMatch best{limit, -1, 1};
    for (int a = 0; a < count; ++a) {
        for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
            const int sign = s ? -1 : 1;
            const float error = scalar_error(target, weight, code + a * stride,
                                                sign, length, best.error);
            if (error < best.error) best = {error, a, sign};
        }
    }
    return best;
}

inline void scalar_beam(const float* target, const float* weight,
        const int16_t* code, int stride, int length, int count, bool signed_code,
        int beam_size, int* indices, int* signs) {
    float errors[32];
    std::fill_n(errors, beam_size, 1.0e30f);
    std::fill_n(indices, beam_size, 0);
    std::fill_n(signs, beam_size, 0);
    for (int a = 0; a < count; ++a) {
        for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
            const int sign = s ? -1 : 1;
            const float error = scalar_error(target, weight, code + a * stride,
                                               sign, length, errors[beam_size - 1]);
            if (error >= errors[beam_size - 1]) continue;
            for (int slot = 0; slot < beam_size; ++slot) {
                if (error >= errors[slot]) continue;
                for (int k = beam_size - 1; k > slot; --k) {
                    errors[k] = errors[k - 1];
                    indices[k] = indices[k - 1];
                    signs[k] = signs[k - 1];
                }
                errors[slot] = error; indices[slot] = a; signs[slot] = sign;
                break;
            }
        }
    }
}
#if defined(TWINVQ_X86)
// SIMD evaluates independent terms; scalar accumulation preserves the old
// rounding order and the pruning check after every four terms. No FMA/reduction.
TWINVQ_INLINE_TARGET("sse4.1") float sse41_error_inline(const float* target, const float* weight,
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

TWINVQ_INLINE_TARGET("avx2") float avx2_error_inline(const float* target, const float* weight,
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

TWINVQ_TARGET("sse4.1") inline float sse41_error(const float* target, const float* weight,
        const int16_t* code, int sign, int length, float limit) {
    return sse41_error_inline(target, weight, code, sign, length, limit);
}

TWINVQ_TARGET("avx2") inline float avx2_error(const float* target, const float* weight,
        const int16_t* code, int sign, int length, float limit) {
    return avx2_error_inline(target, weight, code, sign, length, limit);
}

TWINVQ_TARGET("sse4.1") inline CodebookMatch sse41_search(const float* target, const float* weight,
        const int16_t* code, int stride, int length, int count, bool signed_code, float limit) {
    CodebookMatch best{limit, -1, 1};
    for (int a = 0; a < count; ++a) {
        for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
            const int sign = s ? -1 : 1;
            const float error = sse41_error_inline(target, weight, code + a * stride,
                                                sign, length, best.error);
            if (error < best.error) best = {error, a, sign};
        }
    }
    return best;
}

TWINVQ_TARGET("avx2") inline CodebookMatch avx2_search(const float* target, const float* weight,
        const int16_t* code, int stride, int length, int count, bool signed_code, float limit) {
    CodebookMatch best{limit, -1, 1};
    for (int a = 0; a < count; ++a) {
        for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
            const int sign = s ? -1 : 1;
            const float error = avx2_error_inline(target, weight, code + a * stride,
                                                sign, length, best.error);
            if (error < best.error) best = {error, a, sign};
        }
    }
    return best;
}

TWINVQ_TARGET("sse4.1") inline void sse41_beam(const float* target, const float* weight,
        const int16_t* code, int stride, int length, int count, bool signed_code,
        int beam_size, int* indices, int* signs) {
    float errors[32];
    std::fill_n(errors, beam_size, 1.0e30f);
    std::fill_n(indices, beam_size, 0);
    std::fill_n(signs, beam_size, 0);
    for (int a = 0; a < count; ++a) {
        for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
            const int sign = s ? -1 : 1;
            const float error = sse41_error_inline(target, weight, code + a * stride,
                                               sign, length, errors[beam_size - 1]);
            if (error >= errors[beam_size - 1]) continue;
            for (int slot = 0; slot < beam_size; ++slot) {
                if (error >= errors[slot]) continue;
                for (int k = beam_size - 1; k > slot; --k) {
                    errors[k] = errors[k - 1];
                    indices[k] = indices[k - 1];
                    signs[k] = signs[k - 1];
                }
                errors[slot] = error; indices[slot] = a; signs[slot] = sign;
                break;
            }
        }
    }
}

TWINVQ_TARGET("avx2") inline void avx2_beam(const float* target, const float* weight,
        const int16_t* code, int stride, int length, int count, bool signed_code,
        int beam_size, int* indices, int* signs) {
    float errors[32];
    std::fill_n(errors, beam_size, 1.0e30f);
    std::fill_n(indices, beam_size, 0);
    std::fill_n(signs, beam_size, 0);
    for (int a = 0; a < count; ++a) {
        for (int s = 0; s < (signed_code ? 2 : 1); ++s) {
            const int sign = s ? -1 : 1;
            const float error = avx2_error_inline(target, weight, code + a * stride,
                                               sign, length, errors[beam_size - 1]);
            if (error >= errors[beam_size - 1]) continue;
            for (int slot = 0; slot < beam_size; ++slot) {
                if (error >= errors[slot]) continue;
                for (int k = beam_size - 1; k > slot; --k) {
                    errors[k] = errors[k - 1];
                    indices[k] = indices[k - 1];
                    signs[k] = signs[k - 1];
                }
                errors[slot] = error; indices[slot] = a; signs[slot] = sign;
                break;
            }
        }
    }
}

TWINVQ_INLINE_TARGET("avx2") __m256 avx2_add_candidate_bin(__m256 errors,
        float target, float weight, const float* values) {
    const __m256 d = _mm256_sub_ps(_mm256_set1_ps(target), _mm256_loadu_ps(values));
    const __m256 term = _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(weight), d), d);
    return _mm256_add_ps(errors, term);
}

// Fast path for complete groups of eight candidates.
TWINVQ_INLINE_TARGET("avx2") __m256 avx2_candidate_errors8(const float* target,
        const float* weight, const float* values, int length, float limit) {
    __m256 errors = _mm256_setzero_ps();
    const __m256 cutoff = _mm256_set1_ps(limit);
    int j = 0;
    for (; j + 4 <= length; j += 4) {
        errors = avx2_add_candidate_bin(errors, target[j], weight[j], values + j * 8);
        errors = avx2_add_candidate_bin(errors, target[j+1], weight[j+1], values + (j+1) * 8);
        errors = avx2_add_candidate_bin(errors, target[j+2], weight[j+2], values + (j+2) * 8);
        errors = avx2_add_candidate_bin(errors, target[j+3], weight[j+3], values + (j+3) * 8);
        if (!_mm256_movemask_ps(_mm256_cmp_ps(errors, cutoff, _CMP_LT_OQ))) return errors;
    }
    for (; j < length; ++j)
        errors = avx2_add_candidate_bin(errors, target[j], weight[j], values + j * 8);
    return errors;
}

// Each lane preserves scalar bin order; the mask excludes padded candidates.
TWINVQ_INLINE_TARGET("avx2") __m256 avx2_candidate_errors(const float* target,
        const float* weight, const float* values, int length, int lanes, float limit) {
    const int mask = (1 << lanes) - 1;
    __m256 errors = _mm256_setzero_ps();
    const __m256 cutoff = _mm256_set1_ps(limit);
    int j = 0;
    // Unroll exactly one pruning interval, preserving every lane's original
    // accumulation order and the same cutoff boundaries. No reassociation/FMA.
    for (; j + 4 <= length; j += 4) {
        errors = avx2_add_candidate_bin(errors, target[j], weight[j], values + j * 8);
        errors = avx2_add_candidate_bin(errors, target[j+1], weight[j+1], values + (j+1) * 8);
        errors = avx2_add_candidate_bin(errors, target[j+2], weight[j+2], values + (j+2) * 8);
        errors = avx2_add_candidate_bin(errors, target[j+3], weight[j+3], values + (j+3) * 8);
        if (!(_mm256_movemask_ps(_mm256_cmp_ps(errors, cutoff, _CMP_LT_OQ)) & mask)) return errors;
    }
    for (; j < length; ++j)
        errors = avx2_add_candidate_bin(errors, target[j], weight[j], values + j * 8);
    return errors;
}

// Each lane sums one candidate in scalar bin order. A group may do extra
// arithmetic because its shared cutoff is updated only after ordered selection.
TWINVQ_TARGET("avx2") inline CodebookMatch avx2_candidates(const float* target,
        const float* weight, const PackedCodebook& book, int length, int count,
        bool signed_code, float limit) {
    CodebookMatch best{limit, -1, 1};
    const float* packed = signed_code ? book.signed_values.data() : book.positive.data();
    const int entries = count * (signed_code ? 2 : 1);
    if ((entries & 7) == 0) {
        for (int first = 0; first < entries; first += 8) {
            const float* values = packed + (first / 8) * book.stride * 8;
            const __m256 group_errors = avx2_candidate_errors8(
                target, weight, values, length, best.error);
            const int better_mask = _mm256_movemask_ps(_mm256_cmp_ps(
                group_errors, _mm256_set1_ps(best.error), _CMP_LT_OQ));
            if (!better_mask) continue;
            float error[8];
            _mm256_storeu_ps(error, group_errors);
            for (int lane = 0; lane < 8; ++lane) {
                const int entry = first + lane;
                if (error[lane] < best.error)
                    best = {error[lane], signed_code ? entry / 2 : entry,
                             signed_code && entry % 2 ? -1 : 1};
            }
        }
        return best;
    }
    for (int first = 0; first < entries; first += 8) {
        const int lanes = std::min(8, entries - first);
        const float* values = packed + (first / 8) * book.stride * 8;
        const __m256 group_errors = avx2_candidate_errors(
            target, weight, values, length, lanes, best.error);
        const int lane_mask = (1 << lanes) - 1;
        const int better_mask = _mm256_movemask_ps(_mm256_cmp_ps(
            group_errors, _mm256_set1_ps(best.error), _CMP_LT_OQ)) & lane_mask;
        if (!better_mask) continue;
        float error[8];
        _mm256_storeu_ps(error, group_errors);
        for (int lane = 0; lane < lanes; ++lane) {
            const int entry = first + lane;
            if (error[lane] < best.error)
                best = {error[lane], signed_code ? entry / 2 : entry,
                         signed_code && entry % 2 ? -1 : 1};
        }
    }
    return best;
}

TWINVQ_TARGET("avx2") inline void avx2_candidate_beam(const float* target,
        const float* weight, const PackedCodebook& book, int length, int count,
        bool signed_code, int beam_size, int* indices, int* signs) {
    struct BeamEntry { float error; int index; int sign; };
    BeamEntry beam[32];
    std::fill_n(beam, beam_size, BeamEntry{1.0e30f, 0, 0});
    const float* packed = signed_code ? book.signed_values.data() : book.positive.data();
    const int entries = count * (signed_code ? 2 : 1);
    for (int first = 0; first < entries; first += 8) {
        const int lanes = std::min(8, entries - first);
        const float* values = packed + (first / 8) * book.stride * 8;
        const __m256 group_errors = avx2_candidate_errors(target, weight, values,
                                                          length, lanes, beam[beam_size - 1].error);
        const int lane_mask = (1 << lanes) - 1;
        const int better_mask = _mm256_movemask_ps(_mm256_cmp_ps(
            group_errors, _mm256_set1_ps(beam[beam_size - 1].error), _CMP_LT_OQ)) & lane_mask;
        if (!better_mask) continue;
        float error[8];
        _mm256_storeu_ps(error, group_errors);
        // Stable insertion restores scalar candidate order after parallel scoring.
        for (int lane = 0; lane < lanes; ++lane) {
            if (error[lane] >= beam[beam_size - 1].error) continue;
            const int entry = first + lane;
            const int slot = static_cast<int>(std::upper_bound(beam, beam + beam_size,
                error[lane], [](float value, const BeamEntry& item) { return value < item.error; }) - beam);
            std::move_backward(beam + slot, beam + beam_size - 1, beam + beam_size);
            beam[slot] = {error[lane], signed_code ? entry / 2 : entry,
                          signed_code && entry % 2 ? -1 : 1};
        }
    }
    for (int i = 0; i < beam_size; ++i) {
        indices[i] = beam[i].index;
        signs[i] = beam[i].sign;
    }
}
#endif
} // namespace twinvq::detail

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
