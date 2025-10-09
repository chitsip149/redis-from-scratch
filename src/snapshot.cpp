#include "store.hpp"

#include "score.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {
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
                const char type = "szbc"[entry.data.index()];
                write_all(fd, {&type, 1});
                field(fd, key);
                field(fd, std::to_string(entry.deadline()));
                if (const auto* text = std::get_if<std::string>(&entry.data)) field(fd, *text);
                else if (const auto* filter = std::get_if<BloomFilter>(&entry.data)) field(fd, filter->bits);
                else if (const auto* sketch = std::get_if<CountMinSketch>(&entry.data)) {
                    field(fd, std::to_string(sketch->width()));
                    field(fd, std::to_string(sketch->depth()));
                    field(fd, sketch->encode());
                } else {
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
        if (type != 's' && type != 'z' && type != 'b' && type != 'c')
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
        } else if (type == 'c') {
            long long width = 0, depth = 0;
            if (!resp::integer(field(input), width) || !resp::integer(field(input), depth) ||
                !CountMinSketch::valid_dimensions(width, depth))
                throw std::runtime_error("invalid sketch dimensions in snapshot");
            CountMinSketch sketch(width, depth);
            if (!sketch.decode(field(input))) throw std::runtime_error("invalid sketch counters in snapshot");
            value = std::move(sketch);
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
