# Redis from scratch

A C++17 in-memory key-value server that accepts RESP2 arrays of bulk strings.

```sh
make
./redis-server [port] [workers] [snapshot|-] [keys-per-shard] [idle-seconds]
```

The server listens on IPv4 loopback. The defaults are port 6379, four I/O
threads, four executor threads, no snapshot, no key limit, and no idle timeout. For a
persistent instance with a limit of 100 keys per shard and a 60-second
idle timeout, run `./redis-server 6379 4 dump.snapshot 100 60`. Pass zero
for either limit to disable it. The idle timer measures socket progress
and pauses while a command is in flight.

Run `make test` to check hash-table migration, RESP framing, expiration,
sorted sets, Bloom filters, Count-Min Sketch, and snapshot recovery.

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
- Count-Min Sketch: `CMS.INITBYDIM key width depth`, `CMS.INCRBY key item increment [item increment ...]`,
  and `CMS.QUERY key item [item ...]`. Estimates can overcount because items
  share counters. Dimensions are limited to 32 rows and 131,072 counters
  total. Counters saturate at the signed 64-bit maximum.
- Persistence: `SAVE` writes a snapshot when a path is configured. The server
  also saves after draining queued commands on SIGINT or SIGTERM, and loads
  the snapshot on startup. A crash can lose writes after the last snapshot.
  The versioned snapshot stores binary data and absolute expiration times.

## Source layout

- `server.cpp` starts the server and coordinates shutdown.
- `reactor.cpp`, `events.cpp`, and `connection.cpp` handle I/O scheduling,
  readiness notifications, and per-client request and reply buffers.
- `commands.cpp` interprets RESP commands. `store.cpp` manages the keyspace,
  expiration, and eviction. `snapshot.cpp` reads and writes snapshots.
- The hash table, sorted set, Bloom filter, and Count-Min Sketch each
  have their own header.

## Implementation

The C++ tutorial chapters in the local reference copy informed the
ordered expiration index and rank-aware sorted-set traversal.

I/O threads run level-triggered epoll on Linux and poll on macOS. They
parse incoming requests, hand commands to a bounded executor queue, and
resume socket writes when the executor signals completion through a
socket pair. Each connection has at most one command in flight, preserving
execution and response order for pipelines.

On SIGINT or SIGTERM, I/O threads stop and close client sockets. The
executor then drains submitted jobs and joins its threads before the
optional final snapshot. Clients may disconnect before receiving replies
for those jobs.

The keyspace uses 64 independently locked shards with custom chained hash
tables that resize as they fill. During growth, lookups and deletions
can search both tables while ordinary operations migrate entries in batches. Sorted sets pair a member hash table with
a probabilistic skip list ordered by score and member. Score updates and
deletions use skip-list search. Spans on forward links let rank ranges
jump to the first requested member in expected logarithmic time.

Expired keys are removed on access and through per-shard ordered deadline
indexes. Each background sweep handles at most 128 expired keys. When a
shard reaches its optional key limit, insertion samples up to five entries
and evicts an expired candidate or the least recently accessed candidate.
The limit is per shard and counts keys. Recency is rebuilt after restart.

Snapshot capture holds shard locks for a consistent view. The server
syncs the completed file and atomically renames it into place. The
snapshot format is specific to this server. Key commands take one key,
requests are limited to 1 MiB, and sorted-set replies are capped at 2 MiB.
