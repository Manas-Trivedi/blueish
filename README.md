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
│  len (4B)    │  nargs (4B)  │  [ arglen (4B) | arg (arglen B) ]…  │
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

## Supported Commands

| Command          | Description                              | Response status |
|------------------|------------------------------------------|-----------------|
| `GET <key>`      | Retrieve value for key                   | OK / NX         |
| `SET <key> <val>`| Insert or overwrite a key-value pair     | OK              |
| `DEL <key>`      | Delete a key                             | OK              |

The backing store is currently `std::map<std::string, std::string>` — a placeholder to be replaced with custom hash-map and sorted-set implementations.

## Building

```bash
g++ -o build/server server.cpp -Wall -Wextra -O2
g++ -o build/client client.cpp -Wall -Wextra -O2
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

## Project Structure

```
.
├── server.cpp   # event loop, buffer management, protocol parser, KV store
├── client.cpp   # protocol encoder, interactive REPL, single-shot mode
└── build/
    ├── server
    └── client
```

## Roadmap

- [x] Implement custom buffer arithmetic to replace stl uint8_t vectors
- [ ] Replace `std::map` with a custom open-addressing hash map
- [ ] Sorted sets backed by a skip list or AVL tree
- [ ] TTL / key expiry
- [ ] AOF / RDB persistence
- [ ] Pipelining support (multiple in-flight requests per connection)
- [ ] Benchmarking suite

## Blog

Follow the development of blueis-h [here](https://dev.to/kettlesteam)