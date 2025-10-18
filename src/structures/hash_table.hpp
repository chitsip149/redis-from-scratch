#pragma once

#include <functional>
#include <list>
#include <string>
#include <vector>

template<class Value>
class HashTable {
    using Bucket = std::list<std::pair<std::string, Value>>;
    using Table = std::vector<Bucket>;
    Table buckets_{16};
    Table previous_;
    std::size_t cursor_ = 0;
    std::size_t size_ = 0;

    static std::size_t index(const std::string& key, std::size_t count) {
        return std::hash<std::string>{}(key) % count;
    }

    void migrate() {
        constexpr unsigned budget = 16;
        for (unsigned step = 0; step < budget && !previous_.empty(); ++step) {
            auto& source = previous_[cursor_];
            if (!source.empty()) {
                auto& destination = buckets_[index(source.front().first, buckets_.size())];
                // Splicing keeps pointers to existing entries valid.
                destination.splice(destination.end(), source, source.begin());
            } else if (++cursor_ == previous_.size()) {
                Table{}.swap(previous_);
                cursor_ = 0;
            }
        }
    }

public:
    std::size_t size() const { return size_; }

    Value* find(const std::string& key) {
        migrate();
        for (auto* table : {&buckets_, &previous_}) {
            if (table->empty()) continue;
            for (auto& entry : (*table)[index(key, table->size())])
                if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }

    bool erase(const std::string& key) {
        migrate();
        for (auto* table : {&buckets_, &previous_}) {
            if (table->empty()) continue;
            auto& bucket = (*table)[index(key, table->size())];
            for (auto it = bucket.begin(); it != bucket.end(); ++it) {
                if (it->first == key) {
                    bucket.erase(it);
                    --size_;
                    return true;
                }
            }
        }
        return false;
    }

    Value& insert(std::string key, Value value) {
        if (auto* current = find(key)) {
            *current = std::move(value);
            return *current;
        }
        if (previous_.empty() && size_ >= buckets_.size()) {
            Table larger(buckets_.size() * 2);
            previous_.swap(buckets_);
            buckets_.swap(larger);
        }
        auto& bucket = buckets_[index(key, buckets_.size())];
        bucket.emplace_front(std::move(key), std::move(value));
        ++size_;
        return bucket.front().second;
    }

    template<class Visitor>
    void each(Visitor visitor) {
        for (auto* table : {&buckets_, &previous_})
            for (auto& bucket : *table)
                for (auto& entry : bucket) visitor(entry.first, entry.second);
    }

    template<class Generator, class Visitor>
    void sample(Generator& random, Visitor visitor) {
        const auto total = buckets_.size() + previous_.size();
        const auto start = random() % total;
        unsigned remaining = 5;
        for (std::size_t i = 0; i < total; ++i) {
            const auto slot = (start + i) % total;
            auto& bucket = slot < buckets_.size() ? buckets_[slot] : previous_[slot - buckets_.size()];
            for (auto& entry : bucket) {
                visitor(entry.first, entry.second);
                if (--remaining == 0) return;
            }
        }
    }
};
