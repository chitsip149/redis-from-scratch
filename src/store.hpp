#pragma once

#include "resp.hpp"

#include <array>
#include <cctype>
#include <limits>
#include <mutex>
#include <unordered_map>

class Store {
    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, std::string> values;
    };
    std::array<Shard, 64> shards_;

public:
    std::string execute(const std::vector<std::string_view>& args) {
        std::string name(args.front());
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        constexpr auto arity = "-ERR wrong number of arguments\r\n";
        if (name == "PING") {
            if (args.size() == 1) return "+PONG\r\n";
            return args.size() == 2 ? resp::bulk(args[1]) : arity;
        }
        if (name == "ECHO") return args.size() == 2 ? resp::bulk(args[1]) : arity;
        if (name != "GET" && name != "SET" && name != "DEL" &&
            name != "EXISTS" && name != "INCR") return "-ERR unknown command\r\n";
        if (args.size() != (name == "SET" ? 3U : 2U)) return arity;

        std::string key(args[1]);
        auto& shard = shards_[std::hash<std::string>{}(key) % shards_.size()];
        std::lock_guard lock(shard.mutex);
        auto& values = shard.values;
        const auto item = values.find(key);
        if (name == "GET") return item == values.end() ? "$-1\r\n" : resp::bulk(item->second);
        if (name == "SET") {
            values.insert_or_assign(std::move(key), std::string(args[2]));
            return "+OK\r\n";
        }
        if (name == "DEL") return resp::number(values.erase(key));
        if (name == "EXISTS") return resp::number(item != values.end());

        long long number = 0;
        if (item != values.end() &&
            (!resp::integer(item->second, number) || number == std::numeric_limits<long long>::max()))
            return "-ERR value is not an integer or out of range\r\n";
        values.insert_or_assign(std::move(key), std::to_string(++number));
        return resp::number(number);
    }
};
