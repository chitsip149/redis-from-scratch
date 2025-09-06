#include "store.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {
constexpr auto arity = "-ERR wrong number of arguments\r\n";
constexpr auto bad_integer = "-ERR value is not an integer or out of range\r\n";
constexpr auto wrong_type = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

bool parse_score(std::string_view text, double& value) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    return (input >> std::noskipws >> value) && input.eof() && std::isfinite(value);
}

std::string format_score(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

void io_check(bool success, const char* message) {
    if (!success) throw std::system_error(errno, std::generic_category(), message);
}

void write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto n = write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        io_check(n > 0, "write snapshot");
        data.remove_prefix(n);
    }
}

constexpr std::string_view magic = "RedisFromScratch1\n";
}

long long Store::now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Store::Store(std::string path, std::size_t capacity)
    : path_(std::move(path)), capacity_(capacity) {
    if (!path_.empty()) load();
}

Store::Entry& Store::put(Shard& shard, const std::string& key, Data value, long long deadline) {
    const std::string* victim = nullptr;
    if (capacity_ && shard.values.size() >= capacity_ && !shard.values.find(key)) {
        const auto time = now();
        const Entry* oldest = nullptr;
        shard.values.sample(shard.random, [&](const auto& candidate, const Entry& entry) {
            if (!oldest || (!expired(*oldest, time) &&
                (expired(entry, time) || entry.access < oldest->access))) {
                victim = &candidate;
                oldest = &entry;
            }
        });
    }
    auto& inserted = shard.values.insert(key, {std::move(value), deadline, ++shard.sequence});
    if (victim) shard.values.erase(*victim);
    return inserted;
}

void Store::sweep(std::size_t index) {
    auto& shard = shards_[index % shards_.size()];
    std::lock_guard lock(shard.mutex);
    const auto time = now();
    shard.values.erase_if([&](const Entry& entry) { return expired(entry, time); });
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
    } else if (name == "ZADD" || name == "ZRANGE") expected = 4;
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
        shard.values.erase(key);
        entry = nullptr;
    }
    if (entry) entry->access = ++shard.sequence;
    if (name == "DEL") return resp::number(shard.values.erase(key));
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
        if (deadline == time) shard.values.erase(key);
        else entry->expires = deadline;
        return ":1\r\n";
    }
    if (name == "TTL" || name == "PTTL") {
        if (!entry) return ":-2\r\n";
        if (!entry->expires) return ":-1\r\n";
        return resp::number((entry->expires - time) / (name == "TTL" ? 1000 : 1));
    }
    if (name == "PERSIST") {
        if (!entry || !entry->expires) return ":0\r\n";
        entry->expires = 0;
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
        if (removed && !(*set)->size()) shard.values.erase(key);
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

namespace {
void field(int fd, std::string_view value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    char length[4];
    for (unsigned i = 0; i < 4; ++i) length[i] = static_cast<char>(size >> (24 - 8 * i));
    write_all(fd, {length, sizeof length});
    write_all(fd, value);
}

std::string field(std::istream& input) {
    unsigned char length[4];
    if (!input.read(reinterpret_cast<char*>(length), sizeof length))
        throw std::runtime_error("truncated snapshot");
    std::size_t size = 0;
    for (auto byte : length) size = (size << 8) | byte;
    if (size > resp::max_request) throw std::runtime_error("invalid snapshot field size");
    std::string value(size, '\0');
    if (!input.read(value.data(), value.size())) throw std::runtime_error("truncated snapshot");
    return value;
}
}

void Store::save() {
    std::lock_guard serial(save_mutex_);
    std::string temporary = path_ + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    io_check(fd >= 0, "create snapshot");
    bool open = true;
    try {
        write_all(fd, magic);
        {
            std::vector<std::unique_lock<std::mutex>> locks;
            for (auto& shard : shards_) locks.emplace_back(shard.mutex);
            const auto time = now();
            for (auto& shard : shards_) shard.values.each([&](const auto& key, const Entry& entry) {
                if (expired(entry, time)) return;
                const char type = "szb"[entry.data.index()];
                write_all(fd, {&type, 1});
                field(fd, key);
                field(fd, std::to_string(entry.expires));
                if (const auto* text = std::get_if<std::string>(&entry.data)) field(fd, *text);
                else if (const auto* filter = std::get_if<BloomFilter>(&entry.data)) field(fd, filter->bits);
                else {
                    const auto& set = *std::get<std::unique_ptr<SortedSet>>(entry.data);
                    field(fd, std::to_string(set.size()));
                    set.range(0, set.size(), [&](double score, const std::string& member) {
                        field(fd, member);
                        field(fd, format_score(score));
                        return true;
                    });
                }
            });
        }
        write_all(fd, ".");
        io_check(fsync(fd) == 0, "sync snapshot");
        open = false;
        io_check(close(fd) == 0, "close snapshot");
        io_check(rename(temporary.c_str(), path_.c_str()) == 0, "publish snapshot");
        const auto slash = path_.find_last_of('/');
        const auto parent = slash == path_.npos ? "." : path_.substr(0, slash + 1);
        const int directory = ::open(parent.c_str(), O_RDONLY);
        io_check(directory >= 0, "open snapshot directory");
        const int result = fsync(directory);
        const int error = errno;
        close(directory);
        errno = error;
        io_check(result == 0, "sync snapshot directory");
    } catch (...) {
        if (open) close(fd);
        unlink(temporary.c_str());
        throw;
    }
}

void Store::load() {
    errno = 0;
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (errno == ENOENT) return;
        throw std::system_error(errno, std::generic_category(), "open snapshot");
    }
    std::string header(magic.size(), '\0');
    if (!input.read(header.data(), header.size()) || header != magic)
        throw std::runtime_error("invalid snapshot header");
    const auto time = now();
    for (int type = input.get(); type != '.'; type = input.get()) {
        if (type != 's' && type != 'z' && type != 'b')
            throw std::runtime_error("invalid snapshot entry");
        const auto key = field(input);
        long long deadline = 0;
        if (!resp::integer(field(input), deadline) || deadline < 0)
            throw std::runtime_error("invalid snapshot expiration");
        Data value;
        if (type == 's') value = field(input);
        else if (type == 'b') {
            BloomFilter filter;
            filter.bits = field(input);
            if (filter.bits.size() != BloomFilter::bytes) throw std::runtime_error("invalid Bloom bitmap");
            value = std::move(filter);
        } else {
            long long count = 0;
            if (!resp::integer(field(input), count) || count <= 0)
                throw std::runtime_error("invalid sorted set size");
            auto set = std::make_unique<SortedSet>();
            for (long long i = 0; i < count; ++i) {
                const auto member = field(input);
                double score = 0;
                if (!parse_score(field(input), score) || !set->add(member, score))
                    throw std::runtime_error("invalid sorted set entry");
            }
            value = std::move(set);
        }
        if (!deadline || deadline > time) {
            auto& shard = shards_[std::hash<std::string>{}(key) % shards_.size()];
            put(shard, key, std::move(value), deadline);
        }
    }
    if (input.peek() != EOF || input.bad()) throw std::runtime_error("invalid snapshot trailer");
}
