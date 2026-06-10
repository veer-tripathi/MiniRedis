# redis_modular

A Redis-like in-memory database built from scratch in C++17. Supports strings, lists, and sorted sets with TTL expiry, Pub/Sub messaging, AOF persistence, and background AOF compaction via a threadpool.

---

## Table of Contents

- [Building](#building)
- [Running](#running)
- [Using the Client](#using-the-client)
- [Commands](#commands)
  - [String Commands](#string-commands)
  - [Key Commands](#key-commands)
  - [TTL Commands](#ttl-commands)
  - [List Commands](#list-commands)
  - [Sorted Set Commands](#sorted-set-commands)
  - [Pub/Sub Commands](#pubsub-commands)
  - [Persistence Commands](#persistence-commands)
- [Persistence and AOF](#persistence-and-aof)
- [Testing](#testing)
- [Project Structure](#project-structure)

---

## Building

**Requirements:** g++ with C++17 support, POSIX system (Linux/macOS)

```bash
make
```

This produces three binaries:

| Binary       | Purpose                          |
|--------------|----------------------------------|
| `server`     | The database server              |
| `client`     | Interactive command-line client  |
| `subscriber` | Pub/Sub subscriber helper        |

To clean and rebuild from scratch:

```bash
make clean && make
```

---

## Running

Start the server (listens on port 1234):

```bash
./server
```

The server starts in the foreground and logs to stderr. On startup it automatically replays `appendonly.aof` if it exists, restoring all previous state.

---

## Using the Client

Send a single command:

```bash
./client set mykey hello
./client get mykey
```

The client connects to `127.0.0.1:1234`, sends the command, prints the response, and exits.

### Response format

| Prefix    | Meaning               | Example              |
|-----------|-----------------------|----------------------|
| `(nil)`   | Key not found / null  | `(nil)`              |
| `(str)`   | String value          | `(str) "hello"`      |
| `(int)`   | Integer               | `(int) 1`            |
| `(dbl)`   | Float / double        | `(dbl) 3.14`         |
| `(arr)`   | Array / list          | `(arr) len=3 ...`    |
| `(err)`   | Error                 | `(err) unknown cmd`  |

---

## Commands

### String Commands

#### SET
```
set <key> <value>
```
Store a string value. Overwrites any existing value for the key.
```bash
./client set name "alice"
# (nil)
```

#### GET
```
get <key>
```
Retrieve a string value. Returns `(nil)` if the key does not exist.
```bash
./client get name
# (str) "alice"

./client get nosuchkey
# (nil)
```

---

### Key Commands

#### DEL
```
del <key>
```
Delete a key. Returns `1` if the key existed, `0` if it did not.
```bash
./client del name
# (int) 1

./client del name
# (int) 0
```

#### KEYS
```
keys
```
List all keys that are currently alive (not expired). No arguments.
```bash
./client keys
# (arr) len=3
#   (str) "name"
#   (str) "age"
#   (str) "city"
# (arr) end
```

---

### TTL Commands

#### EXPIRE
```
expire <key> <seconds>
```
Set a key to expire after `seconds` milliseconds from now. Returns `1` on success, `0` if the key does not exist.
```bash
./client set session token123
./client expire session 5000
# (int) 1
```

#### TTL
```
ttl <key>
```
Returns the remaining time to live in milliseconds.

| Return value | Meaning                          |
|--------------|----------------------------------|
| `>= 0`       | Milliseconds until expiry        |
| `-1`         | Key exists but has no TTL        |
| `-2`         | Key does not exist               |

```bash
./client ttl session
# (int) 4823

./client ttl name
# (int) -1

./client ttl nosuchkey
# (int) -2
```

#### PERSIST
```
persist <key>
```
Remove the TTL from a key, making it permanent. Returns `1` if the TTL was removed, `0` if the key has no TTL or does not exist.
```bash
./client persist session
# (int) 1

./client ttl session
# (int) -1
```

---

### List Commands

Lists are ordered sequences of strings. Elements can be added and removed from both ends.

#### RPUSH
```
rpush <key> <value>
```
Append a value to the right (tail) of a list. Returns the new list length.
```bash
./client rpush mylist a
# (int) 1
./client rpush mylist b
# (int) 2
./client rpush mylist c
# (int) 3
```

#### LPUSH
```
lpush <key> <value>
```
Prepend a value to the left (head) of a list. Returns the new list length.
```bash
./client lpush mylist z
# (int) 4
# list is now: z a b c
```

#### LPOP
```
lpop <key>
```
Remove and return the leftmost element. Returns `(nil)` if the list is empty or does not exist. The key is automatically deleted when the list becomes empty.
```bash
./client lpop mylist
# (str) "z"
```

#### RPOP
```
rpop <key>
```
Remove and return the rightmost element.
```bash
./client rpop mylist
# (str) "c"
```

#### LLEN
```
llen <key>
```
Return the number of elements in the list. Returns `0` for a missing key.
```bash
./client llen mylist
# (int) 2
```

#### LRANGE
```
lrange <key> <start> <stop>
```
Return elements from index `start` to `stop` (inclusive). Negative indices count from the tail: `-1` is the last element, `-2` is second to last, etc.
```bash
./client lrange mylist 0 -1
# (arr) len=2
#   (str) "a"
#   (str) "b"
# (arr) end

./client lrange mylist 0 0
# (arr) len=1
#   (str) "a"
# (arr) end
```

---

### Sorted Set Commands

Sorted sets store unique members each with an associated float score. Members are always kept in ascending score order.

#### ZADD
```
zadd <key> <score> <member>
```
Add a member with a score. If the member already exists, its score is updated. Returns `1` if the member was new, `0` if it was updated.
```bash
./client zadd leaderboard 100 alice
# (int) 1
./client zadd leaderboard 200 bob
# (int) 1
./client zadd leaderboard 150 carol
# (int) 1

./client zadd leaderboard 300 alice
# (int) 0  (updated)
```

#### ZSCORE
```
zscore <key> <member>
```
Return the score of a member. Returns `(nil)` if the member or key does not exist.
```bash
./client zscore leaderboard alice
# (dbl) 300

./client zscore leaderboard nobody
# (nil)
```

#### ZREM
```
zrem <key> <member>
```
Remove a member from the sorted set. Returns `1` if removed, `0` if not found.
```bash
./client zrem leaderboard bob
# (int) 1
```

#### ZQUERY
```
zquery <key> <score> <member> <offset> <limit>
```
Range query: find members with score >= `score` (breaking ties by `member` lexicographically), skip `offset` results, return up to `limit` members with their scores.

Returns an interleaved array of `member, score` pairs.
```bash
./client zquery leaderboard 0 "" 0 10
# (arr) len=4
#   (str) "carol"
#   (dbl) 150
#   (str) "alice"
#   (dbl) 300
# (arr) end
```

---

### Pub/Sub Commands

Pub/Sub uses a separate subscriber binary. A subscriber connection enters a special mode where it can only receive messages.

#### SUBSCRIBE
Start the subscriber in one terminal:
```bash
./subscriber sports
# [subscribed] channel=sports count=1
```

#### PUBLISH
In another terminal, publish a message to a channel:
```bash
./client publish sports "goal scored"
# (int) 1   ← number of subscribers that received the message
```

The subscriber terminal will print:
```
[message] channel=sports msg=goal scored
```

#### PUBLISH to empty channel
If no subscribers are listening, publish returns `0`:
```bash
./client publish emptychannel hello
# (int) 0
```

---

### Persistence Commands

#### BGREWRITEAOF
```
bgrewriteaof
```
Trigger AOF compaction in the background. The current AOF is rewritten as the minimal set of commands needed to reproduce live state. Deleted keys, overwritten values, and operation history are discarded. The event loop continues serving requests while compaction runs.
```bash
./client bgrewriteaof
# (str) "Background append only file rewriting started"
```

---

## Persistence and AOF

Every write command is immediately appended to `appendonly.aof` and fsynced to disk. On restart, the server replays this file to restore exact state including TTLs.

### How TTLs survive restarts

TTLs are stored as absolute monotonic timestamps in the AOF rather than relative offsets. This means if a key was set to expire in 10 seconds and the server restarts 3 seconds later, it will correctly expire in the remaining 7 seconds rather than getting a fresh 10 seconds.

### AOF growth and compaction

Without compaction, the AOF grows forever — every SET, DEL, RPUSH ever issued is in the file. Running `bgrewriteaof` replaces it with only the commands needed for current live state:

```
# Before compaction (100 lines of history)
set counter 1
set counter 2
...
set counter 100
del temp_key

# After compaction (1 line)
set counter 100
```

---

## Testing

Run the full test suite (81 tests across all features):

```bash
bash test.sh
```

Or use the Makefile shortcut:

```bash
make test
```

Expected output:

```
========================================
 redis_modular test suite
========================================

[1] String commands
[2] TTL commands
[3] KEYS command
[4] List commands
[5] Sorted-set commands
[6] Pub/Sub
[7] AOF persistence
[8] Error handling
[9] AOF compaction

========================================
 Results: 81/81 passed
========================================

All tests passed.
```

---

## Project Structure

```
redis_modular/
├── Makefile
├── test.sh
├── client.cpp
├── subscriber.cpp
└── src/
    ├── networking/
    │   ├── event_loop.cpp     # main loop, poll(), connection lifecycle
    │   ├── socket_io.cpp      # read/write handlers, request dispatch
    │   ├── socket_io.h
    │   └── connection.h       # Conn struct, connection state machine
    ├── storage/
    │   ├── commands.cpp       # all command handlers (GET, SET, ZADD ...)
    │   ├── commands.h
    │   ├── avl.cpp/h          # AVL tree for sorted sets
    │   ├── h_map.cpp/h        # chained hashmap
    │   ├── heap.h             # min-heap for TTL expiry
    │   └── z_set.cpp/h        # sorted set built on AVL + hashmap
    ├── persistence/
    │   ├── persistence.cpp    # AOF append, replay, compaction
    │   └── persistence.h
    ├── threadpool/
    │   ├── threadpool.cpp     # worker threads, task queue
    │   └── threadpool.h
    ├── protocol/
    │   ├── protocol.cpp/h     # request parsing
    │   └── serializer.cpp/h   # response serialization
    ├── timers/
    │   └── timers.h           # CLOCK_MONOTONIC, intrusive DList
    └── utils/
        ├── buffer.cpp/h       # byte buffer
        ├── logging.cpp/h      # die(), msg()
        └── common.h           # container_of macro
```
