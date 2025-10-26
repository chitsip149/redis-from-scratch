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

namespace detail {
inline Status read_length(std::string_view input, std::size_t& cursor,
                          char prefix, long long& length) {
    if (cursor == input.size()) return Status::incomplete;
    if (input[cursor] != prefix) return Status::invalid;
    const auto end = input.find("\r\n", cursor + 1);
    if (end == input.npos)
        return input.size() - cursor > 24 ? Status::invalid : Status::incomplete;
    if (!integer(input.substr(cursor + 1, end - cursor - 1), length) || length < 0)
        return Status::invalid;
    cursor = end + 2;
    return Status::complete;
}

inline Status read_bulk_string(std::string_view input, std::size_t& cursor,
                               std::string_view& value) {
    long long length = 0;
    const auto status = read_length(input, cursor, '$', length);
    if (status != Status::complete) return status;
    if (cursor > max_request || static_cast<unsigned long long>(length) + cursor + 2 > max_request)
        return Status::invalid;
    const auto end = cursor + static_cast<std::size_t>(length);
    if (input.size() < end + 2) return Status::incomplete;
    if (input.substr(end, 2) != "\r\n") return Status::invalid;
    value = input.substr(cursor, length);
    cursor = end + 2;
    return Status::complete;
}

inline std::string line(char prefix, std::string_view value) {
    std::string result(1, prefix);
    result += value;
    result += "\r\n";
    return result;
}
}

// Commands are RESP2 arrays of non-null bulk strings.
inline Status parse(std::string_view input, std::vector<std::string_view>& args,
                    std::size_t& consumed) {
    std::size_t cursor = 0;
    long long count = 0;
    const auto header = detail::read_length(input, cursor, '*', count);
    if (header != Status::complete) return header;
    if (count == 0 || count > 1024) return Status::invalid;
    for (long long i = 0; i < count; ++i) {
        std::string_view value;
        const auto status = detail::read_bulk_string(input, cursor, value);
        if (status != Status::complete) return status;
        args.push_back(value);
    }
    consumed = cursor;
    return Status::complete;
}

inline std::string simple_string(std::string_view value) {
    return detail::line('+', value);
}

inline std::string error(std::string_view message) {
    return detail::line('-', message);
}

inline std::string bulk(std::string_view value) {
    return detail::line('$', std::to_string(value.size())) + std::string(value) + "\r\n";
}

inline std::string null_bulk() {
    return "$-1\r\n";
}

// The caller appends exactly count encoded values after the header.
inline std::string array_header(std::size_t count) {
    return detail::line('*', std::to_string(count));
}

inline void append_number(std::string& output, long long value) {
    char digits[20];
    const auto end = std::to_chars(digits, digits + sizeof digits, value).ptr;
    output += ':';
    output.append(digits, end);
    output += "\r\n";
}

inline std::string number(long long value) {
    std::string result;
    append_number(result, value);
    return result;
}
}
