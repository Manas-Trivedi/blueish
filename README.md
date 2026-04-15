# blueis-h

An in-memory key-value store built from scratch in C/C++, inspired by Redis. The project focuses on learning systems internals by implementing non-blocking I/O, a custom binary protocol, manual buffer management, and an intrusive hash table instead of relying on heavyweight abstractions.

## Architecture

The server is single-threaded and event-driven, using `poll(2)` to multiplex all client sockets without blocking.

Each connection is put into non-blocking mode with `fcntl(O_NONBLOCK)` and carries a small state machine:

- `want_read`
- `want_write`
- `want_close`

These flags decide which events `poll` watches and how the connection transitions after read/write progress.

## Wire Protocol

The protocol is length-prefixed and binary. All integers are encoded in little-endian form.

### Request frame

```
┌──────────────┬──────────────┬──────────────────────────────────────┐
│  len (4B)    │  nargs (4B)  │  [ arglen (4B) | arg (arglen B) ]…   │
└──────────────┴──────────────┴──────────────────────────────────────┘
```

- `len` is the byte length of the request body that follows.
- `nargs` is the number of command arguments.
- Each argument is encoded as `arglen + raw bytes`.

Example request for `set name blueish`:

```
len=27
nargs=3
arg0="set"
arg1="name"
arg2="blueish"
```

### Response frame

Responses are now TLV-style values inside a length-prefixed frame. There is no separate status-code field and no `RES_NX`-style response anymore.

```
┌──────────────┬────────────────────────────────────┐
│  len (4B)    │   typed value payload (len bytes)  │
└──────────────┴────────────────────────────────────┘
```

The first byte of the payload is a type tag:

| Tag | Name      | Payload layout                    |
|-----|-----------|-----------------------------------|
| `0` | `NIL`     | no payload                        |
| `1` | `ERR`     | currently encoded as empty error  |
| `2` | `STR`     | `len (4B)` + string bytes         |
| `3` | `INT`     | signed integer (`8B`)             |
| `4` | `DBL`     | reserved / not emitted currently  |
| `5` | `ARR`     | `count (4B)` + nested TLV values  |

Examples:

- `GET` hit -> `STR`
- `GET` miss -> `NIL`
- `SET` -> `NIL`
- `DEL` -> `INT(1)` if deleted, `INT(0)` if the key was absent
- `KEYS` -> `ARR` of `STR`
- invalid command -> `ERR`

Current limits:

- max message size: `32 MiB` on the server (`32 << 20`)
- max argument count: `200000`

## Buffer Management

Each connection owns separate `incoming` and `outgoing` buffers. A `Buffer` is a heap-allocated sliding window:

```
[ buffer_begin ... data_begin ... data_end ... buffer_end ]
```

When appending:

1. Live data is compacted back to `buffer_begin` with `memmove` if the tail is full.
2. If there is still not enough room, the buffer is reallocated and grown exponentially.

This keeps allocations low while still allowing variable-sized requests and responses.

## Hash Map Internals

The key-value store uses a custom intrusive hash map from `hashtable.h/.cpp`.

- Each `Entry` embeds its `HNode` directly, avoiding separate node allocations.
- Keys are hashed with MurmurHash3 (`x86_32`).
- Buckets are separate-chaining linked lists.
- Bucket count stays a power of two, so indexing is a simple bit-mask.
- Rehashing is incremental: the map maintains `older` and `newer` tables and migrates a bounded amount of work per operation.
- Lookups and deletes probe both tables while migration is in progress.

This avoids long stop-the-world rehash pauses.

## Supported Commands

| Command | Description | Response |
|---------|-------------|----------|
| `GET <key>` | Fetch a value | `STR` on hit, `NIL` on miss |
| `SET <key> <value>` | Insert or overwrite a value | `NIL` |
| `DEL <key>` | Delete a key | `INT(1)` if deleted, else `INT(0)` |
| `KEYS` | Return all keys | `ARR` of `STR` |

The backing store is an intrusive `HMap` with incremental rehashing.

## Building

```bash
mkdir -p build
g++ -o build/server server.cpp hashtable.cpp -Wall -Wextra -O2
g++ -o build/client client.cpp -Wall -Wextra -O2
g++ -o build/benchmark benchmark.cpp -Wall -Wextra -O2 -pthread
```

## Usage

### Start the server

The server listens on `0.0.0.0:1234`.

```bash
./build/server
```

### Interactive client

```bash
./build/client
> SET name blueish
(nil)
> GET name
blueish
> KEYS
[name]
> DEL name
1
> GET name
(nil)
> EXIT
```

Supported interactive commands:

- `GET <key>`
- `SET <key> <value>`
- `DEL <key>`
- `KEYS`
- `HELP`
- `EXIT`

### Single-shot mode

```bash
./build/client set name blueish
./build/client get name
./build/client del name
./build/client keys
```

### Benchmark mode

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

- throughput in ops/sec for `SET insert`, `SET overwrite`, `GET hit`, and `DEL hit`
- average latency plus `p50`, `p95`, `p99`, min, and max latency in microseconds
- error counts per phase
- a README-ready summary line
- optional Redis `SET`/`GET` ops/sec comparison via `redis-benchmark`

## Project Structure

```
.
├── server.cpp      # event loop, parser, TLV encoder, KV command handlers
├── hashtable.h     # intrusive hash table interfaces (HNode/HTab/HMap)
├── hashtable.cpp   # hash table implementation + incremental rehashing
├── client.cpp      # request encoder, TLV response printer, REPL
├── benchmark.cpp   # concurrent benchmark runner + optional Redis comparison
└── build/
    ├── server
    ├── client
    └── benchmark
```

## Roadmap

- [x] Implement custom buffer arithmetic to replace STL byte-vector buffering
- [x] Replace `std::map` with a custom intrusive hash map
- [x] Move responses to a TLV-style typed protocol
- [ ] Sorted sets backed by a skip list or AVL tree
- [ ] TTL / key expiry
- [ ] AOF / RDB persistence
- [ ] Pipelining support (multiple in-flight requests per connection)
- [x] Benchmarking suite

## Blog

Follow the development of blueis-h [here](https://dev.to/kettlesteam/series/35630)
