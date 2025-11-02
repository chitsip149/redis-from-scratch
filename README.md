# Redis from scratch

A C++17 in-memory key-value server that speaks RESP2. It uses non-blocking sockets with epoll on Linux and poll on macOS.

## Quick start

```sh
make
./redis-server
```

The server listens on `127.0.0.1:6379`. In another terminal, try:

```sh
redis-cli SET counter 10
redis-cli INCR counter
redis-cli GET counter
```

Run `make test` to run the regression tests.

## Options

```text
./redis-server [port] [workers] [snapshot|-] [keys-per-shard] [idle-seconds] [clients-per-reactor]
```

By default, the server uses four I/O threads and four command workers. Snapshots, eviction, and idle timeouts are disabled. Use `-` to skip snapshots and `0` to disable either limit.

For example, `./redis-server 6379 4 dump.snapshot 100 60` enables snapshots, limits each shard to 100 keys, and closes idle connections after 60 seconds.

Each I/O thread admits up to 256 clients by default. Set `clients-per-reactor` to a positive value to change that limit. The cap applies separately to each reactor. Actual capacity also depends on how the shared listener distributes clients and on the process file descriptor limit.

## Supported commands

| Feature | Commands |
| --- | --- |
| Strings and keys | `PING`, `ECHO`, `GET`, `SET`, `INCR`, `DEL`, `EXISTS` |
| Expiration | `SET EX/PX`, `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST` |
| Sorted sets | `ZADD`, `ZSCORE`, `ZREM`, `ZCARD`, `ZRANGE` |
| Bloom filters | `BF.ADD`, `BF.EXISTS` |
| Count-Min Sketch | `CMS.INITBYDIM`, `CMS.INCRBY`, `CMS.QUERY` |
| Snapshots | `SAVE` |

## How it works

I/O threads parse requests and pass them to a separate command pool. Each connection runs one command at a time to preserve pipeline order. A worker posts its reply and wakes the reactor through a socket pair so the reactor can write the response.

The store uses 64 independently locked shards, custom hash tables with incremental resizing, and skip lists for sorted sets. Commands that target the same shard share its lock, while commands targeting different shards can run concurrently. The store supports per-key expiration and sampled LRU eviction.

With snapshots enabled, the server restores data on startup and saves on `SAVE` or after draining submitted work on shutdown. A crash can lose writes made after the last snapshot.

## Source layout

- `src/app/` starts the server and runs the command pool.
- `src/net/` handles connections and socket events.
- `src/protocol/` parses and encodes RESP2.
- `src/storage/` handles commands, keys, and snapshots.
- `src/structures/` contains the custom data structures.
