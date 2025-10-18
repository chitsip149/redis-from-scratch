#include "store.hpp"

long long Store::now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Store::Store(std::string path, std::size_t capacity)
    : path_(std::move(path)), capacity_(capacity) {
    if (!path_.empty()) load();
}

void Store::set_deadline(Shard& shard, const std::string& key, Entry& entry, long long deadline) {
    std::optional<Timers::iterator> replacement;
    if (deadline) replacement = shard.timers.emplace(deadline, key);
    if (entry.timer) shard.timers.erase(*entry.timer);
    entry.timer = replacement;
}

bool Store::erase(Shard& shard, const std::string& key) {
    auto* entry = shard.values.find(key);
    if (!entry) return false;
    if (entry->timer) shard.timers.erase(*entry->timer);
    return shard.values.erase(key);
}

Store::Entry& Store::put(Shard& shard, const std::string& key, Data value, long long deadline) {
    if (auto* existing = shard.values.find(key)) {
        set_deadline(shard, key, *existing, deadline);
        existing->data = std::move(value);
        existing->access = ++shard.sequence;
        return *existing;
    }
    const std::string* victim = nullptr;
    if (capacity_ && shard.values.size() >= capacity_) {
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
    Entry entry{std::move(value), {}, ++shard.sequence};
    set_deadline(shard, key, entry, deadline);
    try {
        auto& inserted = shard.values.insert(key, std::move(entry));
        if (victim) erase(shard, *victim);
        return inserted;
    } catch (...) {
        if (entry.timer) shard.timers.erase(*entry.timer);
        throw;
    }
}

void Store::sweep(std::size_t index) {
    auto& shard = shards_[index % shards_.size()];
    std::lock_guard lock(shard.mutex);
    const auto time = now();
    constexpr std::size_t budget = 128;
    for (std::size_t i = 0; i < budget && !shard.timers.empty(); ++i) {
        const auto next = shard.timers.begin();
        if (next->first > time) break;
        // Erase the value before invalidating the timer's copy of the key.
        shard.values.erase(next->second);
        shard.timers.erase(next);
    }
}
