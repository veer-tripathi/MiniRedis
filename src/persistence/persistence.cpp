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
    // No spaces — write as-is
    if (tok.find(' ') == std::string::npos &&
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
                do_request(cmd, &discard, nullptr);
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