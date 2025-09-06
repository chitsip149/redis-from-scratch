#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class BloomFilter {
public:
    static constexpr std::size_t bytes = 8192;
    std::string bits = std::string(bytes, '\0');

    bool probe(std::string_view item, bool add) {
        std::uint64_t first = 14695981039346656037ULL;
        std::uint64_t step = 5381;
        for (unsigned char byte : item) {
            first = (first ^ byte) * 1099511628211ULL;
            step = step * 33 ^ byte;
        }
        step |= 1;
        bool present = true;
        for (unsigned i = 0; i < 7; ++i) {
            const auto bit = (first + i * step) % (bytes * 8);
            const auto mask = 1U << (bit % 8);
            auto& byte = bits[bit / 8];
            const auto value = static_cast<unsigned char>(byte);
            if (!(value & mask)) present = false;
            if (add) byte = static_cast<char>(value | mask);
        }
        return present;
    }
};
