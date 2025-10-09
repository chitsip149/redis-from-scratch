#include "store.hpp"

#include <cassert>
#include <cstdio>
#include <set>
#include <thread>
#include <unistd.h>

namespace {
void hash_growth() {
    HashTable<int> table;
    for (int i = 0; i < 1025; ++i) table.insert(std::to_string(i), i);
    // The insertion at 1024 starts a resize. Visit both tables before lookup
    // or removal gets a chance to advance the migration.
    std::set<std::string> keys;
    table.each([&](const std::string& key, int value) {
        assert(keys.insert(key).second);
        assert(key == std::to_string(value));
    });
    assert(keys.size() == table.size());

    auto* retained = table.find("1000");
    assert(retained && *retained == 1000);
    assert(table.erase("999"));
    table.insert("998", -1);
    for (int i = 0; i < 1025; ++i) {
        const auto* found = table.find(std::to_string(i));
        if (i == 999) assert(!found);
        else assert(found && *found == (i == 998 ? -1 : i));
    }
    assert(table.find("1000") == retained);
}

void resp_framing() {
    constexpr char wire[] = "*2\r\n$4\r\nECHO\r\n$3\r\na\0b\r\n";
    const std::string_view request(wire, sizeof wire - 1);
    std::vector<std::string_view> args;
    std::size_t consumed = 0;
    assert(resp::parse(request.substr(0, request.size() - 1), args, consumed) ==
           resp::Status::incomplete);
    args.clear();
    assert(resp::parse(request, args, consumed) == resp::Status::complete);
    assert(consumed == request.size() && args.at(1) == std::string_view("a\0b", 3));
    args.clear();
    assert(resp::parse("*1\r\n$3\r\nabcxx", args, consumed) == resp::Status::invalid);
}

void store_round_trip(const std::string& path) {
    {
        Store store(path, 0);
        assert(store.execute({"SET", "ttl", "old", "PX", "1"}) == "+OK\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        assert(store.execute({"GET", "ttl"}) == "$-1\r\n");
        assert(store.execute({"SET", "value", "new"}) == "+OK\r\n");
        assert(store.execute({"ZADD", "set", "3", "b"}) == ":1\r\n");
        assert(store.execute({"ZADD", "set", "2", "a"}) == ":1\r\n");
        assert(store.execute({"ZADD", "set", "1", "b"}) == ":0\r\n");
        assert(store.execute({"ZRANGE", "set", "1", "1"}) == "*1\r\n$1\r\na\r\n");
        assert(store.execute({"BF.ADD", "filter", "item"}) == ":1\r\n");
        assert(store.execute({"CMS.INITBYDIM", "bad", "131072", "2"}).substr(0, 4) == "-ERR");
        assert(store.execute({"CMS.INITBYDIM", "counts", "128", "4"}) == "+OK\r\n");
        assert(store.execute({"CMS.INCRBY", "counts", "a", "3", "b", "4"}) == "*2\r\n:3\r\n:4\r\n");
        assert(store.execute({"CMS.INCRBY", "counts", "a", "99", "b", "-1"}).substr(0, 4) == "-ERR");
        assert(store.execute({"CMS.QUERY", "counts", "a", "b"}) == "*2\r\n:3\r\n:4\r\n");
        assert(store.execute({"CMS.INITBYDIM", "collision", "1", "4"}) == "+OK\r\n");
        assert(store.execute({"CMS.INCRBY", "collision", "a", "9223372036854775807", "b", "1"}) ==
               "*2\r\n:9223372036854775807\r\n:9223372036854775807\r\n");
        assert(store.execute({"SAVE"}) == "+OK\r\n");
    }
    Store restored(path, 0);
    assert(restored.execute({"GET", "value"}) == "$3\r\nnew\r\n");
    assert(restored.execute({"GET", "ttl"}) == "$-1\r\n");
    assert(restored.execute({"ZRANGE", "set", "0", "-1"}) == "*2\r\n$1\r\nb\r\n$1\r\na\r\n");
    assert(restored.execute({"CMS.QUERY", "counts", "a", "b"}) == "*2\r\n:3\r\n:4\r\n");
    assert(restored.execute({"CMS.QUERY", "collision", "unseen"}) == "*1\r\n:9223372036854775807\r\n");
    assert(restored.execute({"BF.EXISTS", "filter", "item"}) == ":1\r\n");
}
}

int main() {
    hash_growth();
    resp_framing();
    std::string directory = "/tmp/redis-regression.XXXXXX";
    if (!mkdtemp(directory.data())) {
        std::perror("mkdtemp");
        return 1;
    }
    const auto path = directory + "/store.snapshot";
    store_round_trip(path);
    unlink(path.c_str());
    rmdir(directory.c_str());
}
