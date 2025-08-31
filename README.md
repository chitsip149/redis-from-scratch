# Redis from scratch

A C++17 in-memory key-value server that accepts RESP2 arrays of bulk strings.
Build with `make` and run `./redis-server [port] [workers]`. The defaults
are port 6379 and four workers. The server listens on IPv4 loopback.

Each worker runs a level-triggered epoll loop on Linux and uses poll on
macOS. Workers accept connections from a shared non-blocking listener.
The accepting worker owns that connection and processes its requests in
wire order. Incomplete requests stay buffered across reads, and partial
replies resume when the socket is writable.

The store hashes keys across 64 shards, each with its own mutex. Workers
can execute operations on separate shards concurrently. Supported commands
are PING, ECHO, GET, SET, DEL, EXISTS, and INCR. The key commands operate
on a single key. Values are binary-safe and live in memory until shutdown.
Requests are limited to 1 MiB, with a 2 MiB pending reply limit per
connection. SIGINT and SIGTERM stop the workers and close their sockets.

```sh
redis-cli SET counter 10
redis-cli INCR counter
redis-cli GET counter
```
