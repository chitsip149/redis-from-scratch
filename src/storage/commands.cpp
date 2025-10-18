#include "store.hpp"

#include "score.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace {
constexpr auto arity = "-ERR wrong number of arguments\r\n";
constexpr auto bad_integer = "-ERR value is not an integer or out of range\r\n";
constexpr auto wrong_type = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
}

std::string Store::execute(const std::vector<std::string_view>& args) {
    std::string name(args.front());
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto count = args.size();
    if (name == "PING") {
        if (count == 1) return "+PONG\r\n";
        return count == 2 ? resp::bulk(args[1]) : arity;
    }
    if (name == "ECHO") return count == 2 ? resp::bulk(args[1]) : arity;
    if (name == "SAVE") {
        if (count != 1) return arity;
        if (path_.empty()) return "-ERR snapshot path is disabled\r\n";
        save();
        return "+OK\r\n";
    }

    std::size_t expected = 2;
    if (name == "SET") {
        if (count != 3 && count != 5) return arity;
        expected = count;
    } else if (name == "CMS.INCRBY" || name == "CMS.QUERY") {
        if (count < 3 || (name == "CMS.INCRBY" && (count < 4 || count % 2))) return arity;
        expected = count;
    } else if (name == "ZADD" || name == "ZRANGE" || name == "CMS.INITBYDIM") expected = 4;
    else if (name == "EXPIRE" || name == "PEXPIRE" || name == "ZSCORE" ||
             name == "ZREM" || name == "BF.ADD" || name == "BF.EXISTS") expected = 3;
    else if (name != "GET" && name != "DEL" && name != "EXISTS" && name != "INCR" &&
             name != "TTL" && name != "PTTL" && name != "PERSIST" && name != "ZCARD")
        return "-ERR unknown command\r\n";
    if (count != expected) return arity;

    const std::string key(args[1]);
    auto& shard = shards_[std::hash<std::string>{}(key) % shards_.size()];
    std::lock_guard lock(shard.mutex);
    const auto time = now();
    auto* entry = shard.values.find(key);
    if (entry && expired(*entry, time)) {
        erase(shard, key);
        entry = nullptr;
    }
    if (entry) entry->access = ++shard.sequence;
    if (name == "DEL") return resp::number(erase(shard, key));
    if (name == "EXISTS") return resp::number(entry != nullptr);
    if (name == "SET" || name == "EXPIRE" || name == "PEXPIRE") {
        const bool set = name == "SET";
        const bool seconds = name == "EXPIRE" || (set && count == 5 && (args[3] == "EX" || args[3] == "ex"));
        const long long scale = seconds ? 1000 : 1;
        long long deadline = 0;
        if (!set || count == 5) {
            if (set && !seconds && args[3] != "PX" && args[3] != "px")
                return "-ERR syntax error\r\n";
            long long duration = 0;
            if (!resp::integer(args[set ? 4 : 2], duration) ||
                duration > (std::numeric_limits<long long>::max() - time) / scale ||
                (set && duration <= 0)) return bad_integer;
            deadline = duration <= 0 ? time : time + duration * scale;
        }
        if (set) {
            put(shard, key, std::string(args[2]), deadline);
            return "+OK\r\n";
        }
        if (!entry) return ":0\r\n";
        if (deadline == time) erase(shard, key);
        else set_deadline(shard, key, *entry, deadline);
        return ":1\r\n";
    }
    if (name == "TTL" || name == "PTTL") {
        if (!entry) return ":-2\r\n";
        if (!entry->timer) return ":-1\r\n";
        return resp::number((entry->deadline() - time) / (name == "TTL" ? 1000 : 1));
    }
    if (name == "PERSIST") {
        if (!entry || !entry->timer) return ":0\r\n";
        set_deadline(shard, key, *entry, 0);
        return ":1\r\n";
    }
    if (name == "GET" || name == "INCR") {
        auto* value = entry ? std::get_if<std::string>(&entry->data) : nullptr;
        if (entry && !value) return wrong_type;
        if (name == "GET") return value ? resp::bulk(*value) : "$-1\r\n";
        long long number = 0;
        if (value && (!resp::integer(*value, number) || number == std::numeric_limits<long long>::max()))
            return bad_integer;
        if (value) *value = std::to_string(++number);
        else put(shard, key, std::to_string(++number));
        return resp::number(number);
    }
    if (name == "BF.ADD" || name == "BF.EXISTS") {
        const bool add = name == "BF.ADD";
        if (!entry && !add) return ":0\r\n";
        if (!entry) entry = &put(shard, key, BloomFilter{});
        auto* filter = std::get_if<BloomFilter>(&entry->data);
        if (!filter) return wrong_type;
        const bool present = filter->probe(args[2], add);
        return resp::number(add ? !present : present);
    }

    if (name == "CMS.INITBYDIM") {
        if (entry) return "-ERR key already exists\r\n";
        long long width = 0, depth = 0;
        if (!resp::integer(args[2], width) || !resp::integer(args[3], depth) ||
            !CountMinSketch::valid_dimensions(width, depth))
            return "-ERR invalid sketch dimensions\r\n";
        put(shard, key, CountMinSketch(width, depth));
        return "+OK\r\n";
    }
    if (name == "CMS.INCRBY" || name == "CMS.QUERY") {
        if (!entry) return "-ERR sketch does not exist\r\n";
        auto* sketch = std::get_if<CountMinSketch>(&entry->data);
        if (!sketch) return wrong_type;
        const bool update = name == "CMS.INCRBY";
        const std::size_t items = (count - 2) / (update ? 2 : 1);
        std::vector<long long> increments;
        if (update) {
            increments.resize(items);
            for (std::size_t i = 0; i < items; ++i) {
                if (!resp::integer(args[3 + 2 * i], increments[i]) || increments[i] < 0)
                    return "-ERR increment must be a non-negative integer\r\n";
            }
        }
        std::string result = "*" + std::to_string(items) + "\r\n";
        result.reserve(result.size() + 22 * items);
        for (std::size_t i = 0; i < items; ++i) {
            const auto estimate = update ? sketch->increment(args[2 + 2 * i], increments[i])
                                         : sketch->query(args[2 + i]);
            char digits[20];
            const auto end = std::to_chars(digits, digits + sizeof digits, estimate).ptr;
            result += ':';
            result.append(digits, end);
            result += "\r\n";
        }
        return result;
    }

    auto* set = entry ? std::get_if<std::unique_ptr<SortedSet>>(&entry->data) : nullptr;
    if (entry && !set) return wrong_type;
    if (name == "ZADD") {
        double score = 0;
        if (!parse_score(args[2], score)) return "-ERR invalid score\r\n";
        if (!set) set = &std::get<std::unique_ptr<SortedSet>>(put(shard, key, std::make_unique<SortedSet>()).data);
        return resp::number((*set)->add(std::string(args[3]), score));
    }
    if (name == "ZCARD") return resp::number(set ? (*set)->size() : 0);
    if (name == "ZREM") {
        const bool removed = set && (*set)->remove(std::string(args[2]));
        if (removed && !(*set)->size()) erase(shard, key);
        return resp::number(removed);
    }
    if (name == "ZSCORE") {
        const auto* score = set ? (*set)->score(std::string(args[2])) : nullptr;
        return score ? resp::bulk(format_score(*score)) : "$-1\r\n";
    }

    long long first = 0, last = 0;
    if (!resp::integer(args[2], first) || !resp::integer(args[3], last)) return bad_integer;
    if (!set) return "*0\r\n";
    const auto size = static_cast<long long>((*set)->size());
    first = std::max(0LL, first < 0 ? size + first : first);
    last = std::min(size - 1, last < 0 ? size + last : last);
    if (first > last) return "*0\r\n";
    std::string result = "*" + std::to_string(last - first + 1) + "\r\n";
    bool too_large = false;
    (*set)->range(first, last + 1, [&](double, const std::string& member) {
        result += resp::bulk(member);
        too_large = result.size() > 2 * resp::max_request;
        return !too_large;
    });
    return too_large ? "-ERR reply exceeds size limit\r\n" : result;
}
