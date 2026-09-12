#pragma once

#include <cstdint>
#include <vector>

namespace twinvq {

class BitReader {
public:
    BitReader(const uint8_t* data, int size_bytes)
        : data_(data), size_bytes_(size_bytes) {}

    int get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; i++)
            v = (v << 1) | get1();
        return static_cast<int>(v);
    }

    int get1() {
        if (bit_index_ >= size_bytes_ * 8)
            return 0;
        const int byte = data_[bit_index_ >> 3];
        const int bit = 7 - (bit_index_ & 7);
        bit_index_++;
        return (byte >> bit) & 1;
    }

    void skip(int n) { bit_index_ += n; }

    int bits_read() const { return bit_index_; }

private:
    const uint8_t* data_;
    int size_bytes_;
    int bit_index_ = 0;
};

class BitWriter {
public:
    void put(int n, unsigned v) {
        for (int i = n - 1; i >= 0; i--)
            put1((v >> i) & 1u);
    }

    void put1(unsigned bit) {
        if ((bit_index_ & 7) == 0)
            bytes_.push_back(0);
        if (bit)
            bytes_[bit_index_ >> 3] |= static_cast<uint8_t>(1u << (7 - (bit_index_ & 7)));
        bit_index_++;
    }

    int bits_written() const { return bit_index_; }

    const std::vector<uint8_t>& bytes() const { return bytes_; }

    std::vector<uint8_t> take_bytes() {
        // Pad to a whole byte.
        while (bit_index_ & 7)
            put1(0);
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    int bit_index_ = 0;
};

} // namespace twinvq
