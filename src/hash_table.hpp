#pragma once

#include <functional>
#include <list>
#include <string>
#include <vector>

template<class Value>
class HashTable {
    using Bucket = std::list<std::pair<std::string, Value>>;
    std::vector<Bucket> buckets_{16};
    std::size_t size_ = 0;

    static std::size_t index(const std::string& key, std::size_t count) {
        return std::hash<std::string>{}(key) % count;
    }

public:
    std::size_t size() const { return size_; }

    Value* find(const std::string& key) {
        for (auto& entry : buckets_[index(key, buckets_.size())])
            if (entry.first == key) return &entry.second;
        return nullptr;
    }

    bool erase(const std::string& key) {
        auto& bucket = buckets_[index(key, buckets_.size())];
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if (it->first == key) {
                bucket.erase(it);
                --size_;
                return true;
            }
        }
        return false;
    }

    Value& insert(std::string key, Value value) {
        if (auto* current = find(key)) {
            *current = std::move(value);
            return *current;
        }
        if (size_ >= buckets_.size()) {
            std::vector<Bucket> larger(buckets_.size() * 2);
            for (auto& bucket : buckets_) {
                while (!bucket.empty()) {
                    auto& dest = larger[index(bucket.front().first, larger.size())];
                    dest.splice(dest.end(), bucket, bucket.begin());
                }
            }
            buckets_.swap(larger);
        }
        auto& bucket = buckets_[index(key, buckets_.size())];
        bucket.emplace_front(std::move(key), std::move(value));
        ++size_;
        return bucket.front().second;
    }

    template<class Visitor>
    void each(Visitor visitor) {
        for (auto& bucket : buckets_)
            for (auto& entry : bucket) visitor(entry.first, entry.second);
    }

    template<class Generator, class Visitor>
    void sample(Generator& random, Visitor visitor) {
        const auto start = random() % buckets_.size();
        unsigned remaining = 5;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            for (auto& entry : buckets_[(start + i) % buckets_.size()]) {
                visitor(entry.first, entry.second);
                if (--remaining == 0) return;
            }
        }
    }
};
