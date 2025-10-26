#include "store.hpp"

#include "score.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace {
constexpr auto arity = "ERR wrong number of arguments";
constexpr auto bad_integer = "ERR value is not an integer or out of range";
constexpr auto wrong_type = "WRONGTYPE Operation against a key holding the wrong kind of value";
}

std::string Store::execute(const std::vector<std::string_view>& args) {
    std::string name(args.front());
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto count = args.size();
    if (name == "PING") {
        if (count == 1) return resp::simple_string("PONG");
        return count == 2 ? resp::bulk(args[1]) : resp::error(arity);
    }
    if (name == "ECHO") return count == 2 ? resp::bulk(args[1]) : resp::error(arity);
    if (name == "SAVE") {
        if (count != 1) return resp::error(arity);
        if (path_.empty()) return resp::error("ERR snapshot path is disabled");
        save();
        return resp::simple_string("OK");
    }

    std::size_t expected = 2;
    if (name == "SET") {
        if (count != 3 && count != 5) return resp::error(arity);
        expected = count;
    } else if (name == "CMS.INCRBY" || name == "CMS.QUERY") {
        if (count < 3 || (name == "CMS.INCRBY" && (count < 4 || count % 2))) return resp::error(arity);
        expected = count;
    } else if (name == "ZADD" || name == "ZRANGE" || name == "CMS.INITBYDIM") expected = 4;
    else if (name == "EXPIRE" || name == "PEXPIRE" || name == "ZSCORE" ||
             name == "ZREM" || name == "BF.ADD" || name == "BF.EXISTS") expected = 3;
    else if (name != "GET" && name != "DEL" && name != "EXISTS" && name != "INCR" &&
             name != "TTL" && name != "PTTL" && name != "PERSIST" && name != "ZCARD")
        return resp::error("ERR unknown command");
    if (count != expected) return resp::error(arity);

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
                return resp::error("ERR syntax error");
            long long duration = 0;
            if (!resp::integer(args[set ? 4 : 2], duration) ||
                duration > (std::numeric_limits<long long>::max() - time) / scale ||
                (set && duration <= 0)) return resp::error(bad_integer);
            deadline = duration <= 0 ? time : time + duration * scale;
        }
        if (set) {
            put(shard, key, std::string(args[2]), deadline);
            return resp::simple_string("OK");
        }
        if (!entry) return resp::number(0);
        if (deadline == time) erase(shard, key);
        else set_deadline(shard, key, *entry, deadline);
        return resp::number(1);
    }
    if (name == "TTL" || name == "PTTL") {
        if (!entry) return resp::number(-2);
        if (!entry->timer) return resp::number(-1);
        return resp::number((entry->deadline() - time) / (name == "TTL" ? 1000 : 1));
    }
    if (name == "PERSIST") {
        if (!entry || !entry->timer) return resp::number(0);
        set_deadline(shard, key, *entry, 0);
        return resp::number(1);
    }
    if (name == "GET" || name == "INCR") {
        auto* value = entry ? std::get_if<std::string>(&entry->data) : nullptr;
        if (entry && !value) return resp::error(wrong_type);
        if (name == "GET") return value ? resp::bulk(*value) : resp::null_bulk();
        long long number = 0;
        if (value && (!resp::integer(*value, number) || number == std::numeric_limits<long long>::max()))
            return resp::error(bad_integer);
        if (value) *value = std::to_string(++number);
        else put(shard, key, std::to_string(++number));
        return resp::number(number);
    }
    if (name == "BF.ADD" || name == "BF.EXISTS") {
        const bool add = name == "BF.ADD";
        if (!entry && !add) return resp::number(0);
        if (!entry) entry = &put(shard, key, BloomFilter{});
        auto* filter = std::get_if<BloomFilter>(&entry->data);
        if (!filter) return resp::error(wrong_type);
        const bool present = filter->probe(args[2], add);
        return resp::number(add ? !present : present);
    }

    if (name == "CMS.INITBYDIM") {
        if (entry) return resp::error("ERR key already exists");
        long long width = 0, depth = 0;
        if (!resp::integer(args[2], width) || !resp::integer(args[3], depth) ||
            !CountMinSketch::valid_dimensions(width, depth))
            return resp::error("ERR invalid sketch dimensions");
        put(shard, key, CountMinSketch(width, depth));
        return resp::simple_string("OK");
    }
    if (name == "CMS.INCRBY" || name == "CMS.QUERY") {
        if (!entry) return resp::error("ERR sketch does not exist");
        auto* sketch = std::get_if<CountMinSketch>(&entry->data);
        if (!sketch) return resp::error(wrong_type);
        const bool update = name == "CMS.INCRBY";
        const std::size_t items = (count - 2) / (update ? 2 : 1);
        std::vector<long long> increments;
        if (update) {
            increments.resize(items);
            for (std::size_t i = 0; i < items; ++i) {
                if (!resp::integer(args[3 + 2 * i], increments[i]) || increments[i] < 0)
                    return resp::error("ERR increment must be a non-negative integer");
            }
        }
        std::string result = resp::array_header(items);
        result.reserve(result.size() + 22 * items);
        for (std::size_t i = 0; i < items; ++i) {
            const auto estimate = update ? sketch->increment(args[2 + 2 * i], increments[i])
                                         : sketch->query(args[2 + i]);
            resp::append_number(result, estimate);
        }
        return result;
    }

    auto* set = entry ? std::get_if<std::unique_ptr<SortedSet>>(&entry->data) : nullptr;
    if (entry && !set) return resp::error(wrong_type);
    if (name == "ZADD") {
        double score = 0;
        if (!parse_score(args[2], score)) return resp::error("ERR invalid score");
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
        return score ? resp::bulk(format_score(*score)) : resp::null_bulk();
    }

    long long first = 0, last = 0;
    if (!resp::integer(args[2], first) || !resp::integer(args[3], last)) return resp::error(bad_integer);
    if (!set) return resp::array_header(0);
    const auto size = static_cast<long long>((*set)->size());
    first = std::max(0LL, first < 0 ? size + first : first);
    last = std::min(size - 1, last < 0 ? size + last : last);
    if (first > last) return resp::array_header(0);
    std::string result = resp::array_header(last - first + 1);
    bool too_large = false;
    (*set)->range(first, last + 1, [&](double, const std::string& member) {
        result += resp::bulk(member);
        too_large = result.size() > 2 * resp::max_request;
        return !too_large;
    });
    return too_large ? resp::error("ERR reply exceeds size limit") : result;
}
