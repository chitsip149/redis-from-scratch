#pragma once

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace resp {
constexpr std::size_t max_request = 1024 * 1024;
enum class Status { incomplete, complete, invalid };

inline bool integer(std::string_view text, long long& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Commands are RESP2 arrays of non-null bulk strings.
inline Status parse(std::string_view input, std::vector<std::string_view>& args,
                    std::size_t& consumed) {
    std::size_t cursor = 0;
    auto header = [&](char prefix, long long& size) {
        if (cursor == input.size()) return Status::incomplete;
        if (input[cursor] != prefix) return Status::invalid;
        const auto end = input.find("\r\n", cursor + 1);
        if (end == input.npos)
            return input.size() - cursor > 24 ? Status::invalid : Status::incomplete;
        if (!integer(input.substr(cursor + 1, end - cursor - 1), size) || size < 0)
            return Status::invalid;
        cursor = end + 2;
        return Status::complete;
    };
    long long count = 0;
    auto status = header('*', count);
    if (status != Status::complete) return status;
    if (count == 0 || count > 1024) return Status::invalid;
    for (long long i = 0; i < count; ++i) {
        long long length = 0;
        status = header('$', length);
        if (status != Status::complete) return status;
        if (cursor > max_request || static_cast<unsigned long long>(length) + cursor + 2 > max_request)
            return Status::invalid;
        const auto end = cursor + static_cast<std::size_t>(length);
        if (input.size() < end + 2) return Status::incomplete;
        if (input.substr(end, 2) != "\r\n") return Status::invalid;
        args.push_back(input.substr(cursor, length));
        cursor = end + 2;
    }
    consumed = cursor;
    return Status::complete;
}

inline std::string bulk(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

inline std::string number(long long value) {
    return ":" + std::to_string(value) + "\r\n";
}
}
