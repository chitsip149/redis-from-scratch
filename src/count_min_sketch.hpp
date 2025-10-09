#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

class CountMinSketch {
    static constexpr long long maximum = std::numeric_limits<long long>::max();
    std::size_t width_;
    std::vector<long long> counters_;

    static std::uint64_t hash(std::string_view item) {
        std::uint64_t value = 14695981039346656037ULL;
        for (unsigned char byte : item) value = (value ^ byte) * 1099511628211ULL;
        return value;
    }

    std::size_t index(std::uint64_t hash, std::size_t row) const {
        // Mix a different seed into each row before choosing its column.
        auto value = hash + 0x9e3779b97f4a7c15ULL * (row + 1);
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return row * width_ + (value ^ (value >> 31)) % width_;
    }

public:
    static constexpr std::size_t max_counters = 131072;
    static constexpr std::size_t max_depth = 32;

    static bool valid_dimensions(long long width, long long depth) {
        return width > 0 && depth > 0 && depth <= static_cast<long long>(max_depth) &&
               width <= static_cast<long long>(max_counters) / depth;
    }

    CountMinSketch(std::size_t width, std::size_t depth)
        : width_(width), counters_(width * depth) {}

    std::size_t width() const { return width_; }
    std::size_t depth() const { return counters_.size() / width_; }

    long long query(std::string_view item) const {
        const auto value = hash(item);
        long long estimate = maximum;
        for (std::size_t row = 0; row < depth(); ++row)
            estimate = std::min(estimate, counters_[index(value, row)]);
        return estimate;
    }

    long long increment(std::string_view item, long long amount) {
        const auto value = hash(item);
        long long estimate = maximum;
        for (std::size_t row = 0; row < depth(); ++row) {
            auto& count = counters_[index(value, row)];
            count += std::min(amount, maximum - count);
            estimate = std::min(estimate, count);
        }
        return estimate;
    }

    std::string encode() const {
        std::string bytes;
        bytes.reserve(counters_.size() * 8);
        for (auto count : counters_)
            for (unsigned shift = 64; shift > 0; shift -= 8)
                bytes.push_back(static_cast<char>(static_cast<std::uint64_t>(count) >> (shift - 8)));
        return bytes;
    }

    bool decode(std::string_view bytes) {
        if (bytes.size() != counters_.size() * 8) return false;
        for (auto& count : counters_) {
            std::uint64_t value = 0;
            for (unsigned char byte : bytes.substr(0, 8)) value = (value << 8) | byte;
            if (value > static_cast<std::uint64_t>(maximum)) return false;
            count = static_cast<long long>(value);
            bytes.remove_prefix(8);
        }
        return true;
    }
};
