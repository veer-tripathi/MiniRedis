#include "persistence.h"
#include "../storage/commands.h"
#include "../storage/z_set.h"
#include "../storage/avl.h"
#include "../utils/common.h"
#include "../utils/logging.h"
#include "../utils/buffer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// File handle — kept open for the lifetime of the server so every
// aof_append() call can write and fsync without reopening.
// ---------------------------------------------------------------------------

static int g_aof_fd = -1;

// ---------------------------------------------------------------------------
// Quoting
// ---------------------------------------------------------------------------
// Tokens that contain spaces are wrapped in double quotes when written.
// On replay, quoted tokens are reassembled into a single string.
// Simple escaping: a literal " inside a token is written as \".

static std::string quote_token(const std::string &tok) {
    // No spaces, no quotes, and non-empty — write as-is
    if (!tok.empty() &&
        tok.find(' ') == std::string::npos &&
        tok.find('"') == std::string::npos)
        return tok;

    // Needs quoting
    std::string out;
    out.reserve(tok.size() + 2);
    out += '"';
    for (char c : tok) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// Parse one AOF line into tokens.
// Handles quoted strings with \" escaping.
// Returns empty vector on blank lines.
static std::vector<std::string> parse_aof_line(const std::string &line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    size_t n = line.size();

    while (i < n) {
        // Skip whitespace
        while (i < n && line[i] == ' ') i++;
        if (i >= n) break;

        std::string tok;

        if (line[i] == '"') {
            // Quoted token — read until closing unescaped "
            i++;  // skip opening quote
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n && line[i + 1] == '"') {
                    tok += '"';
                    i += 2;
                } else if (line[i] == '"') {
                    i++;  // skip closing quote
                    break;
                } else {
                    tok += line[i++];
                }
            }
        } else {
            // Unquoted token — read until space
            while (i < n && line[i] != ' ') tok += line[i++];
        }

        if (!tok.empty()) tokens.push_back(tok);
    }

    return tokens;
}

// ---------------------------------------------------------------------------
// aof_init
// ---------------------------------------------------------------------------

void aof_init(const char *path) {
    // Replay existing file if it exists
    {
        std::ifstream file(path);
        if (file.is_open()) {
            fprintf(stderr, "[AOF] replaying %s\n", path);
            std::string line;
            size_t count = 0;

            while (std::getline(file, line)) {
                if (line.empty()) continue;

                std::vector<std::string> cmd = parse_aof_line(line);
                if (cmd.empty()) continue;

                // Replay through do_request using a throwaway buffer.
                // We don't care about the response during replay —
                // we're just rebuilding state.
                Buffer discard = buf_init();
                // Pass a dummy Conn* — replay commands are never pub/sub
                // (we don't persist subscribe/publish) so conn is unused.
                do_request(cmd, &discard, nullptr, nullptr);
                buf_free(&discard);
                count++;
            }

            fprintf(stderr, "[AOF] replayed %zu commands\n", count);
        }
    }

    // Open the file in append mode, creating it if it doesn't exist.
    // O_WRONLY | O_CREAT | O_APPEND — never truncate, always append.
    g_aof_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_aof_fd < 0) {
        fprintf(stderr, "[AOF] failed to open %s: %s\n", path, strerror(errno));
        die("aof_init: open()");
    }

    fprintf(stderr, "[AOF] writing to %s\n", path);
}

// ---------------------------------------------------------------------------
// aof_append
// ---------------------------------------------------------------------------

void aof_append(const std::vector<std::string> &tokens) {
    if (g_aof_fd < 0) return;  // persistence not initialised

    // Build the line: space-separated quoted tokens + newline
    std::string line;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i > 0) line += ' ';
        line += quote_token(tokens[i]);
    }
    line += '\n';

    // Write the line
    const char *buf = line.data();
    size_t      rem = line.size();
    while (rem > 0) {
        ssize_t rv = write(g_aof_fd, buf, rem);
        if (rv < 0) {
            if (errno == EINTR) continue;
            msg_errno("[AOF] write() failed");
            return;
        }
        buf += rv;
        rem -= (size_t)rv;
    }

    // fsync — flush OS buffers to disk before returning.
    // This guarantees the command is durable before the response
    // goes back to the client.
    if (fsync(g_aof_fd) < 0)
        msg_errno("[AOF] fsync() failed");
}
// ---------------------------------------------------------------------------
// aof_compact
// ---------------------------------------------------------------------------
// Rewrites the AOF as the minimal snapshot of current live state.
//
// Algorithm:
//   1. Walk every live key via db_for_each_entry()
//   2. Emit the minimal commands to recreate it:
//        string  → SET key value
//        list    → RPUSH key e0 e1 ... (one per element)
//        zset    → ZADD key score member (one per member, in score order)
//   3. If the key has a TTL: EXPIREAT key <absolute-monotonic-ms>
//   4. Write to a .tmp file, fsync, then rename() over the live AOF.
//      rename() is atomic on POSIX — no window where the file is missing.
//   5. Reopen g_aof_fd pointing at the new file.
//
// Called from do_bgrewriteaof (BGREWRITEAOF command).  Runs synchronously
// on the main thread (single-threaded server) — safe because nothing else
// mutates state concurrently.

bool aof_compact(const char *path) {
    std::string tmp_path = std::string(path) + ".tmp";

    // Open the temp file (truncate if it somehow already exists)
    int tmp_fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        fprintf(stderr, "[AOF compact] open tmp failed: %s\n", strerror(errno));
        return false;
    }

    // Helper: write a fully-formed line to tmp_fd
    bool write_ok = true;
    auto write_line = [&](const std::vector<std::string> &tokens) {
        if (!write_ok) return;
        std::string line;
        for (size_t i = 0; i < tokens.size(); i++) {
            if (i > 0) line += ' ';
            line += quote_token(tokens[i]);
        }
        line += '\n';
        const char *buf = line.data();
        size_t rem = line.size();
        while (rem > 0) {
            ssize_t rv = write(tmp_fd, buf, rem);
            if (rv < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "[AOF compact] write failed: %s\n", strerror(errno));
                write_ok = false;
                return;
            }
            buf += rv; rem -= (size_t)rv;
        }
    };

    // Snapshot every live key
    db_for_each_entry([&](
        const std::string &key,
        uint32_t           type,
        const std::string &str,
        const std::deque<std::string> &list,
        AVLNode           *zset_root,
        uint64_t           expire_at_ms
    ) {
        if (type == 1) {  // T_STR
            write_line({"set", key, str});

        } else if (type == 3) {  // T_LIST
            for (const std::string &elem : list)
                write_line({"rpush", key, elem});

        } else if (type == 2) {  // T_ZSET — walk AVL tree in ascending order
            // Build a temporary ZSet view to use zset_seek
            // We can't construct a ZSet here, but we own the root pointer.
            // Use avl_offset to walk from the leftmost node (+0 from leftmost).
            // Get leftmost: avl_offset(root, 0) gives the root, but we want
            // the node at rank 0 which is avl_offset(root, -(cnt-1)).
            // Simpler: cast root back — we know it's a ZNode.
            if (zset_root) {
                // Walk to the leftmost node
                AVLNode *cur = zset_root;
                while (cur->left) cur = cur->left;
                ZNode *znode = container_of(cur, ZNode, tree);
                while (znode) {
                    std::string name(znode->name, znode->len);
                    std::string score_str = std::to_string(znode->score);
                    write_line({"zadd", key, score_str, name});
                    znode = znode_offset(znode, +1);
                }
            }
        }

        // Persist TTL as absolute deadline so it survives restart correctly
        if (expire_at_ms != 0) {
            write_line({"expireat", key, std::to_string(expire_at_ms)});
        }
    });

    if (!write_ok) {
        close(tmp_fd);
        unlink(tmp_path.c_str());
        return false;
    }

    // fsync before rename so the data is durable
    if (fsync(tmp_fd) < 0) {
        fprintf(stderr, "[AOF compact] fsync failed: %s\n", strerror(errno));
        close(tmp_fd);
        unlink(tmp_path.c_str());
        return false;
    }
    close(tmp_fd);

    // Atomic rename — replaces the live AOF with the compacted version
    if (rename(tmp_path.c_str(), path) < 0) {
        fprintf(stderr, "[AOF compact] rename failed: %s\n", strerror(errno));
        unlink(tmp_path.c_str());
        return false;
    }

    // Reopen g_aof_fd pointing at the new file
    int new_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (new_fd < 0) {
        fprintf(stderr, "[AOF compact] reopen failed: %s\n", strerror(errno));
        return false;
    }
    if (g_aof_fd >= 0) close(g_aof_fd);
    g_aof_fd = new_fd;

    fprintf(stderr, "[AOF compact] compaction complete → %s\n", path);
    return true;
}