#pragma once

#include "bloom.hpp"
#include "count_min_sketch.hpp"
#include "resp.hpp"
#include "sorted_set.hpp"

#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <variant>

class Store {
    using Data = std::variant<std::string, std::unique_ptr<SortedSet>, BloomFilter, CountMinSketch>;
    using Timers = std::multimap<long long, std::string>;
    struct Entry {
        Data data;
        std::optional<Timers::iterator> timer;
        long long deadline() const { return timer ? (*timer)->first : 0; }
        std::uint64_t access = 0;
    };
    struct Shard {
        std::mutex mutex;
        HashTable<Entry> values;
        Timers timers;
        std::uint64_t sequence = 0;
        std::minstd_rand random{std::random_device{}()};
    };

    std::array<Shard, 64> shards_;
    std::mutex save_mutex_;
    std::string path_;
    std::size_t capacity_;

    static long long now();
    static bool expired(const Entry& entry, long long time) {
        return entry.timer && entry.deadline() <= time;
    }
    static void set_deadline(Shard& shard, const std::string& key, Entry& entry, long long deadline);
    static bool erase(Shard& shard, const std::string& key);
    Entry& put(Shard& shard, const std::string& key, Data value, long long expires = 0);
    void load();

public:
    Store(std::string path, std::size_t capacity);
    std::string execute(const std::vector<std::string_view>& args);
    void sweep(std::size_t index);
    void save();
};
