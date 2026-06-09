#include "persistence.h"
#include "../storage/commands.h"
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

static int g_aof_fd = -1;

static std::string quote_token(const std::string &tok) {
    // No spaces, no quotes, and non-empty — write as-is
    if (!tok.empty() &&
        tok.find(' ') == std::string::npos &&
        tok.find('"') == std::string::npos)
        return tok;

    // Needs quoting (including empty string → "")
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
                do_request(cmd, &discard, nullptr);
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