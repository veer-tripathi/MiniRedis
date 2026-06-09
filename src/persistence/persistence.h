#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AOF (Append-Only File) persistence
// ---------------------------------------------------------------------------
//
// Every write command is appended to appendonly.aof as a plain text line
// immediately after it executes, followed by an fsync so the data is on
// disk before the response goes back to the client.
//
// On startup, aof_init() replays the file top-to-bottom through the normal
// do_request path, rebuilding exact server state.
//
// File format — one command per line, tokens space-separated, strings with
// internal spaces quoted:
//
//   set foo bar
//   rpush messages "alice: hello world"
//   expire foo 60000
//   del foo
//
// ---------------------------------------------------------------------------

// Open (or create) the AOF file at `path`.
// If the file already exists, replay every line through do_request to
// restore state before the server starts accepting connections.
// Calls die() on unrecoverable errors.
void aof_init(const char *path);

// Append a successfully executed write command to the AOF file and fsync.
// Only call this for commands that mutate state — not for reads.
// tokens is the same cmd vector that was passed to do_request.
void aof_append(const std::vector<std::string> &tokens);