# blueis-h

An in-memory key-value store built from scratch in C/C++, inspired by Redis. The goal is to deeply understand systems design — non-blocking I/O, custom wire protocols, manual memory management, and event-driven concurrency — by implementing them rather than relying on abstractions.

## Architecture

The server is **single-threaded and event-driven**, using `poll(2)` to multiplex I/O across all connections without blocking.

Every file descriptor is set to non-blocking mode via `fcntl(O_NONBLOCK)`. Each connection owns a state machine with three flags — `want_read`, `want_write`, `want_close` — which drive which events `poll` monitors and how I/O callbacks transition state.

## Wire Protocol

A custom binary protocol with length-prefixed framing. All integers are little-endian.

**Request frame:**
```
┌──────────────┬──────────────┬──────────────────────────────────────┐
│  len (4B)    │  nargs (4B)  │  [ arglen (4B) | arg (arglen B) ]…   │
└──────────────┴──────────────┴──────────────────────────────────────┘
```

**Response frame:**
```
┌──────────────┬──────────────┬──────────────────┐
│  len (4B)    │  status (4B) │  body (len-4 B)  │
└──────────────┴──────────────┴──────────────────┘
```

Status codes: `0` = OK, `1` = ERR, `2` = NX (key not found).

Max message size on the server is 32 MiB (`32 << 20`).

## Buffer Management

Each connection has separate `incoming` and `outgoing` `Buffer` structs. A `Buffer` is a heap-allocated sliding window:

```
[ buffer_begin … data_begin … data_end … buffer_end ]
```

On append, if there is insufficient space at the tail:
1. **Compact** — slide live data back to `buffer_begin` via `memmove`.
2. **Reallocate** — if still insufficient, double capacity and `malloc` a new block.

This avoids per-message allocation while keeping memory usage bounded.

## Hash Map Internals

The key-value store now uses a custom intrusive hash map from `hashtable.h/.cpp`.

- Each record embeds an `HNode` directly in the owning object (`Entry`), avoiding extra node allocations.
- Keys are hashed with MurmurHash3 (x86_32) before bucket lookup/insert/delete.
- Buckets are separate-chaining linked lists (`HNode*` chains).
- Bucket count is always a power of two, so indexing uses `hcode & mask`.
- Resizing is incremental: the map keeps `newer` and `older` tables and migrates a bounded amount of work per operation (`k_rehashing_work`).
- Lookups and deletes check both tables while migration is in progress.

This mirrors Redis-style progressive rehashing behavior and avoids long pause times from full-table rehashes.

## Supported Commands

| Command          | Description                              | Response status |
|------------------|------------------------------------------|-----------------|
| `GET <key>`      | Retrieve value for key                   | OK / NX         |
| `SET <key> <val>`| Insert or overwrite a key-value pair     | OK              |
| `DEL <key>`      | Delete a key                             | OK              |

The backing store is an intrusive hash map (`HMap`) with incremental rehashing.

## Building

```bash
g++ -o build/server server.cpp hashtable.cpp -Wall -Wextra -O2
g++ -o build/client client.cpp -Wall -Wextra -O2
g++ -o build/benchmark benchmark.cpp -Wall -Wextra -O2 -pthread
```

## Usage

**Start the server** (listens on `0.0.0.0:1234`):
```bash
./build/server
```

**Interactive client (REPL):**
```bash
./build/client
> SET name redis-clone
> GET name
server says: [0] redis-clone
> DEL name
> EXIT
```

**Single-shot mode:**
```bash
./build/client set name redis-clone
./build/client get name
./build/client del name
```

**Benchmark mode:**
```bash
./build/benchmark --server-cmd "./build/server"
```

Example with a larger run and optional Redis comparison:
```bash
./build/benchmark \
  --server-cmd "./build/server" \
  --connections 64 \
  --requests 100000 \
  --key-count 20000 \
  --value-size 128 \
  --compare-redis
```

The benchmark reports:
- Throughput in ops/sec for `SET insert`, `SET overwrite`, `GET hit`, and `DEL hit`
- Average latency plus `p50`, `p95`, `p99`, min, and max latency in microseconds
- Error counts per phase
- A README-ready summary line such as `Achieved ~X us average GET latency under Y concurrent connections; peak measured throughput reached ~Z ops/sec.`
- Optional Redis `SET`/`GET` ops/sec comparison via `redis-benchmark`

## Project Structure

```
.
├── server.cpp      # event loop, protocol parser, KV commands
├── hashtable.h     # intrusive hash table interfaces (HNode/HTab/HMap)
├── hashtable.cpp   # hash table implementation + incremental rehashing
├── client.cpp      # protocol encoder, interactive REPL, single-shot mode
├── benchmark.cpp   # concurrent benchmark runner + optional Redis comparison
└── build/
    ├── server
    ├── client
    └── benchmark
```

## Roadmap

- [x] Implement custom buffer arithmetic to replace stl uint8_t vectors
- [x] Replace `std::map` with a custom intrusive hash map
- [ ] Sorted sets backed by a skip list or AVL tree
- [ ] TTL / key expiry
- [ ] AOF / RDB persistence
- [ ] Pipelining support (multiple in-flight requests per connection)
- [x] Benchmarking suite

## Blog

Follow the development of blueis-h [here](https://dev.to/kettlesteam/series/35630)
