#pragma once

#include "twinvq/twinvq_types.hpp"
#include "twinvq_mdct.hpp"
#include <array>
#include <cstring>

namespace twinvq {

// Window types are the validated bitstream values 0..8.
inline FrameType window_frame_type(int wtype) {
    constexpr FrameType types[] = {FrameType::Long, FrameType::Long, FrameType::Short,
        FrameType::Long, FrameType::Medium, FrameType::Long, FrameType::Long,
        FrameType::Medium, FrameType::Medium};
    return types[wtype];
}

// Decoder window geometry. An overlap is a rotation of two half-IMDCT
// vectors; the unwindowed tail is retained for the following subblock/frame.
struct WindowBlock {
    int overlap;
    int output;
    int previous;
};

struct WindowLayout {
    int size;
    int blocks;
    int block_size;
    int output_size;
    std::array<WindowBlock, kSubblocksMax> block{};
};

inline WindowLayout window_layout(const ModeTab& mode, FrameType type, int wtype) {
    constexpr int sizes[] = {0, 0, 2, 2, 2, 1, 0, 1, 1};
    const int widths[] = {mode.size, mode.size / mode.fmode[1].sub,
                         mode.size / (2 * mode.fmode[0].sub)};
    WindowLayout layout{};
    layout.size = mode.size;
    layout.blocks = mode.fmode[static_cast<int>(type)].sub;
    layout.block_size = mode.size / layout.blocks;
    layout.output_size = (mode.size + widths[sizes[wtype]]) / 2;
    int out = 0;
    for (int j = 0; j < layout.blocks; ++j) {
        int sub_type = type == FrameType::Medium ? 8 : wtype;
        if (j == 0 && wtype == 4) sub_type = 4;
        else if (j == layout.blocks - 1 && wtype == 7) sub_type = 7;
        const int width = widths[sizes[sub_type]];
        layout.block[j] = {width, out, j == 0 ? (mode.size - width) / 2
                                                             : j * layout.block_size - width / 2};
        out += width + (type == FrameType::Medium ? (layout.block_size - width) / 2
                                                : layout.block_size - width);
    }
    return layout;
}

// previous starts at the previous frame's saved output position, exactly as
// in Decoder::imdct_output. half has size samples, current has 2*size samples.
inline void synthesize_window(const WindowLayout& layout, const float* half,
                              const float* previous, float* current) {
    for (int j = 0; j < layout.blocks; ++j) {
        const auto& b = layout.block[j];
        const float* input = half + j * layout.block_size;
        const float* prev = j == 0 ? previous : half;
        vector_fmul_window(current + b.output, prev + b.previous, input,
                           sine_window_cached(b.overlap), b.overlap / 2);
        std::memcpy(current + b.output + b.overlap, input + b.overlap / 2,
                    (layout.block_size - b.overlap / 2) * sizeof(float));
    }
}

// Unquantized, offline analysis/synthesis oracle for transition development.
bool window_transition_self_test(float* max_abs_err);

} // namespace twinvq
