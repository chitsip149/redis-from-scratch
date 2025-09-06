#pragma once

#include "hash_table.hpp"

#include <array>
#include <memory>
#include <random>
#include <utility>

class SortedSet {
    using Key = std::pair<double, std::string>;
    static constexpr std::size_t levels = 24;
    struct Node {
        Key key;
        std::vector<Node*> next;
        Node(Key value, std::size_t height) : key(std::move(value)), next(height) {}
    };

    Node head_{{}, levels};
    HashTable<double> scores_;
    std::minstd_rand random_{std::random_device{}()};

    std::array<Node*, levels> predecessors(const Key& key) {
        std::array<Node*, levels> path{};
        Node* node = &head_;
        for (std::size_t level = levels; level-- > 0;) {
            while (node->next[level] && node->next[level]->key < key)
                node = node->next[level];
            path[level] = node;
        }
        return path;
    }

    void unlink(const Key& key) {
        const auto path = predecessors(key);
        auto* node = path[0]->next[0];
        for (std::size_t i = 0; i < node->next.size(); ++i) path[i]->next[i] = node->next[i];
        delete node;
    }

public:
    SortedSet() = default;
    SortedSet(const SortedSet&) = delete;
    SortedSet& operator=(const SortedSet&) = delete;
    ~SortedSet() {
        auto* node = head_.next[0];
        while (node) {
            auto* next = node->next[0];
            delete node;
            node = next;
        }
    }

    std::size_t size() const { return scores_.size(); }
    const double* score(const std::string& member) { return scores_.find(member); }

    bool add(const std::string& member, double score) {
        auto* old = scores_.find(member);
        if (old && *old == score) return false;
        std::size_t height = 1;
        while (height < levels && (random() & 3U) == 0) ++height;
        auto node = std::make_unique<Node>(Key{score, member}, height);
        const bool added = old == nullptr;
        if (old) {
            unlink({*old, member});
            *old = score;
        } else {
            scores_.insert(member, score);
        }
        const auto path = predecessors(node->key);
        for (std::size_t i = 0; i < height; ++i) {
            node->next[i] = path[i]->next[i];
            path[i]->next[i] = node.get();
        }
        node.release();
        return added;
    }

    bool remove(const std::string& member) {
        const auto* old = scores_.find(member);
        if (!old) return false;
        unlink({*old, member});
        return scores_.erase(member);
    }

    template<class Visitor>
    void range(std::size_t first, std::size_t end, Visitor visitor) const {
        auto* node = head_.next[0];
        for (std::size_t rank = 0; node && rank < end; ++rank, node = node->next[0])
            if (rank >= first && !visitor(node->key.first, node->key.second)) break;
    }
};
