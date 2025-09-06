# Redis from scratch

A C++17 in-memory key-value server that accepts RESP2 arrays of bulk strings.

```sh
make
./redis-server [port] [workers] [snapshot|-] [keys-per-shard]
```

The server listens on IPv4 loopback. The defaults are port 6379, four I/O
threads, four executor threads, no snapshot, and no key limit. For a
persistent instance with a limit of 100 keys per shard, run
`./redis-server 6379 4 dump.snapshot 100`.

## Commands

- Strings and keys: `PING`, `ECHO`, `GET`, `SET key value [EX seconds|PX milliseconds]`,
  `INCR`, `DEL`, and `EXISTS`.
- Expiration: `EXPIRE key seconds`, `PEXPIRE key milliseconds`, `TTL`, `PTTL`,
  and `PERSIST`.
- Sorted sets: `ZADD key score member`, `ZSCORE key member`, `ZREM key member`,
  `ZCARD key`, and `ZRANGE key start stop`. Ranges use inclusive ranks and
  accept negative indices. Equal scores are ordered by member.
- Bloom filters: `BF.ADD key item` and `BF.EXISTS key item`. Each filter uses
  a fixed 8 KiB bitmap and seven probes. Positive matches can be false
  positives, with the probability increasing as the bitmap fills.
- Persistence: `SAVE` writes a snapshot when a path is configured. The server
  also saves after draining queued commands on SIGINT or SIGTERM, and loads
  the snapshot on startup. A crash can lose writes after the last snapshot.
  The versioned snapshot stores binary data and absolute expiration times.

## Implementation

I/O threads run level-triggered epoll on Linux and poll on macOS. They
parse incoming requests, hand commands to a bounded executor queue, and
resume socket writes when the executor signals completion through a
socket pair. Each connection has at most one command in flight, preserving
execution and response order for pipelines.

The keyspace uses 64 independently locked shards with custom chained hash
tables that resize as they fill. Sorted sets pair a member hash table with
a probabilistic skip list ordered by score and member. Score updates and
deletions use skip-list search. Rank ranges walk the bottom level.

Expired keys are removed on access and by periodic shard sweeps. When a
shard reaches its optional key limit, insertion samples up to five entries
and evicts an expired candidate or the least recently accessed candidate.
The limit is per shard and counts keys. Recency is rebuilt after restart.

Snapshot capture holds shard locks for a consistent view. The server
syncs the completed file and atomically renames it into place. The
snapshot format is specific to this server. Key commands take one key,
requests are limited to 1 MiB, and sorted-set replies are capped at 2 MiB.
