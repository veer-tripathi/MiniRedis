#include "persistence.h"
#include <memory>
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
// File handle
// ---------------------------------------------------------------------------

static int g_aof_fd = -1;

// ---------------------------------------------------------------------------
// Quoting
// ---------------------------------------------------------------------------

static std::string quote_token(const std::string &tok) {
    if (!tok.empty() &&
        tok.find(' ') == std::string::npos &&
        tok.find('"') == std::string::npos)
        return tok;

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

static std::vector<std::string> parse_aof_line(const std::string &line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    size_t n = line.size();

    while (i < n) {
        while (i < n && line[i] == ' ') i++;
        if (i >= n) break;

        std::string tok;

        if (line[i] == '"') {
            i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n && line[i + 1] == '"') {
                    tok += '"';
                    i += 2;
                } else if (line[i] == '"') {
                    i++;
                    break;
                } else {
                    tok += line[i++];
                }
            }
        } else {
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

                Buffer discard = buf_init();
                do_request(cmd, &discard, nullptr, std::weak_ptr<ThreadPool>{});
                buf_free(&discard);
                count++;
            }

            fprintf(stderr, "[AOF] replayed %zu commands\n", count);
        }
    }

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
    if (g_aof_fd < 0) return;

    std::string line;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i > 0) line += ' ';
        line += quote_token(tokens[i]);
    }
    line += '\n';

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

    if (fsync(g_aof_fd) < 0)
        msg_errno("[AOF] fsync() failed");
}

// ---------------------------------------------------------------------------
// aof_compact
// ---------------------------------------------------------------------------

bool aof_compact(const char *path) {
    std::string tmp_path = std::string(path) + ".tmp";

    int tmp_fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        fprintf(stderr, "[AOF compact] open tmp failed: %s\n", strerror(errno));
        return false;
    }

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

        } else if (type == 2) {  // T_ZSET
            if (zset_root) {
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

        if (expire_at_ms != 0) {
            write_line({"expireat", key, std::to_string(expire_at_ms)});
        }
    });

    if (!write_ok) {
        close(tmp_fd);
        unlink(tmp_path.c_str());
        return false;
    }

    if (fsync(tmp_fd) < 0) {
        fprintf(stderr, "[AOF compact] fsync failed: %s\n", strerror(errno));
        close(tmp_fd);
        unlink(tmp_path.c_str());
        return false;
    }
    close(tmp_fd);

    if (rename(tmp_path.c_str(), path) < 0) {
        fprintf(stderr, "[AOF compact] rename failed: %s\n", strerror(errno));
        unlink(tmp_path.c_str());
        return false;
    }

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