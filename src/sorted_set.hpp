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
        struct Link {
            Node* node = nullptr;
            std::size_t span = 0;
        };
        std::vector<Link> next;
        Node(Key value, std::size_t height) : key(std::move(value)), next(height) {}
    };

    Node head_{{}, levels};
    HashTable<double> scores_;
    std::minstd_rand random_{std::random_device{}()};

    struct Path {
        std::array<Node*, levels> nodes{};
        std::array<std::size_t, levels> ranks{};
    };

    Path predecessors(const Key& key) {
        Path path;
        Node* node = &head_;
        std::size_t rank = 0;
        for (std::size_t level = levels; level-- > 0;) {
            while (node->next[level].node && node->next[level].node->key < key) {
                rank += node->next[level].span;
                node = node->next[level].node;
            }
            path.nodes[level] = node;
            path.ranks[level] = rank;
        }
        return path;
    }

    void unlink(const Key& key) {
        const auto path = predecessors(key);
        auto* node = path.nodes[0]->next[0].node;
        for (std::size_t i = 0; i < levels; ++i) {
            auto& link = path.nodes[i]->next[i];
            --link.span;
            if (link.node == node) {
                link.span += node->next[i].span;
                link.node = node->next[i].node;
            }
        }
        delete node;
    }

public:
    SortedSet() = default;
    SortedSet(const SortedSet&) = delete;
    SortedSet& operator=(const SortedSet&) = delete;
    ~SortedSet() {
        auto* node = head_.next[0].node;
        while (node) {
            auto* next = node->next[0].node;
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
        for (std::size_t i = 0; i < levels; ++i) {
            auto& link = path.nodes[i]->next[i];
            if (i < height) {
                const auto skipped = path.ranks[0] - path.ranks[i];
                node->next[i] = {link.node, link.span - skipped};
                link = {node.get(), skipped + 1};
            } else {
                ++link.span;
            }
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
        if (first >= end || first >= size()) return;
        const Node* node = &head_;
        std::size_t traversed = 0;
        for (std::size_t level = levels; level-- > 0;) {
            while (node->next[level].node && node->next[level].span <= first - traversed) {
                traversed += node->next[level].span;
                node = node->next[level].node;
            }
        }
        node = node->next[0].node;
        for (auto rank = first; node && rank < end; ++rank, node = node->next[0].node)
            if (!visitor(node->key.first, node->key.second)) break;
    }
};
