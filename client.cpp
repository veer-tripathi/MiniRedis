// =============================================================================
// client.cpp — command-line client for the redis-like server
//
// Build:   g++ -std=c++17 -o client client.cpp
// Usage:   ./client <command> [args...]
//
// Examples:
//   ./client set foo bar
//   ./client get foo
//   ./client del foo
//   ./client keys
//   ./client zadd myset 1.5 alice
//   ./client zadd myset 2.0 bob
//   ./client zscore myset alice
//   ./client zquery myset 0 "" 0 10
//   ./client zrem myset alice
// =============================================================================

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants — must match the server
// ---------------------------------------------------------------------------

const size_t k_max_msg = 32 << 20;   // 32 MB

// ---------------------------------------------------------------------------
// Response type tags (must match src/protocol/serializer.h)
// ---------------------------------------------------------------------------
enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

// ---------------------------------------------------------------------------
// Utility: fatal error
// ---------------------------------------------------------------------------
static void die(const char *msg) {
    fprintf(stderr, "[errno %d] %s\n", errno, msg);
    abort();
}

// ---------------------------------------------------------------------------
// Reliable read / write helpers
// ---------------------------------------------------------------------------
static int32_t read_full(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;     // error or unexpected EOF
        n   -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        n   -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Send a request
//
// Wire format:
//   [body_len : u32] [nstrs : u32] [len : u32] [str] ... [len : u32] [str]
// ---------------------------------------------------------------------------
static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    // Calculate body length: nstrs field + (len field + bytes) per string
    uint32_t body_len = 4;
    for (const std::string &s : cmd) {
        body_len += 4 + (uint32_t)s.size();
    }

    if (body_len > k_max_msg) {
        fprintf(stderr, "request too large\n");
        return -1;
    }

    // Serialise into a single buffer and write in one call
    std::vector<uint8_t> wbuf;
    wbuf.resize(4 + body_len);

    memcpy(wbuf.data(), &body_len, 4);          // outer frame length

    uint32_t nstrs = (uint32_t)cmd.size();
    memcpy(wbuf.data() + 4, &nstrs, 4);         // number of strings

    size_t cur = 8;
    for (const std::string &s : cmd) {
        uint32_t slen = (uint32_t)s.size();
        memcpy(wbuf.data() + cur, &slen, 4);
        memcpy(wbuf.data() + cur + 4, s.data(), s.size());
        cur += 4 + s.size();
    }

    return write_all(fd, wbuf.data(), wbuf.size());
}

// ---------------------------------------------------------------------------
// Parse and print one tagged value starting at *pos.
// Returns the number of bytes consumed, or -1 on error.
// indent controls pretty-printing for nested arrays.
// ---------------------------------------------------------------------------
static int32_t print_value(const uint8_t *buf, uint32_t len,
                            uint32_t pos, int indent) {
    if (pos >= len) {
        fprintf(stderr, "unexpected end of response\n");
        return -1;
    }

    uint8_t tag = buf[pos];
    pos += 1;

    // Helper: print leading spaces for nested output
    auto pad = [&]() {
        for (int i = 0; i < indent * 2; i++) putchar(' ');
    };

    switch (tag) {

    // ---- NIL ---------------------------------------------------------------
    case TAG_NIL:
        pad(); printf("(nil)\n");
        return 1;

    // ---- STRING ------------------------------------------------------------
    case TAG_STR: {
        if (pos + 4 > len) { fprintf(stderr, "bad string\n"); return -1; }
        uint32_t slen = 0;
        memcpy(&slen, buf + pos, 4);
        pos += 4;
        if (pos + slen > len) { fprintf(stderr, "bad string data\n"); return -1; }
        pad(); printf("(str) \"%.*s\"\n", (int)slen, buf + pos);
        return (int32_t)(1 + 4 + slen);
    }

    // ---- INTEGER -----------------------------------------------------------
    case TAG_INT: {
        if (pos + 8 > len) { fprintf(stderr, "bad int\n"); return -1; }
        int64_t val = 0;
        memcpy(&val, buf + pos, 8);
        pad(); printf("(int) %ld\n", val);
        return 1 + 8;
    }

    // ---- DOUBLE ------------------------------------------------------------
    case TAG_DBL: {
        if (pos + 8 > len) { fprintf(stderr, "bad double\n"); return -1; }
        double val = 0;
        memcpy(&val, buf + pos, 8);
        pad(); printf("(dbl) %g\n", val);
        return 1 + 8;
    }

    // ---- ERROR -------------------------------------------------------------
    case TAG_ERR: {
        if (pos + 8 > len) { fprintf(stderr, "bad err\n"); return -1; }
        uint32_t code = 0, slen = 0;
        memcpy(&code, buf + pos,     4);
        memcpy(&slen, buf + pos + 4, 4);
        pos += 8;
        if (pos + slen > len) { fprintf(stderr, "bad err msg\n"); return -1; }
        pad(); printf("(err) code=%u \"%.*s\"\n", code, (int)slen, buf + pos);
        return (int32_t)(1 + 8 + slen);
    }

    // ---- ARRAY -------------------------------------------------------------
    case TAG_ARR: {
        if (pos + 4 > len) { fprintf(stderr, "bad array\n"); return -1; }
        uint32_t n = 0;
        memcpy(&n, buf + pos, 4);
        pos += 4;
        pad(); printf("(arr) len=%u\n", n);

        int32_t total = 1 + 4;   // tag + count field
        for (uint32_t i = 0; i < n; i++) {
            int32_t consumed = print_value(buf, len, pos, indent + 1);
            if (consumed < 0) return -1;
            pos   += (uint32_t)consumed;
            total += consumed;
        }
        pad(); printf("(arr) end\n");
        return total;
    }

    default:
        fprintf(stderr, "unknown tag: %u\n", tag);
        return -1;
    }
}

// ---------------------------------------------------------------------------
// Read one framed response from the server and print it
// ---------------------------------------------------------------------------
static int32_t read_res(int fd) {
    // 1. Read the 4-byte frame length
    uint8_t lenbuf[4];
    errno = 0;
    if (read_full(fd, lenbuf, 4) != 0) {
        fprintf(stderr, errno ? "read() error\n" : "EOF\n");
        return -1;
    }

    uint32_t body_len = 0;
    memcpy(&body_len, lenbuf, 4);
    if (body_len > k_max_msg) {
        fprintf(stderr, "response too long (%u bytes)\n", body_len);
        return -1;
    }

    // 2. Read the body
    std::vector<uint8_t> body(body_len);
    if (read_full(fd, body.data(), body_len) != 0) {
        fprintf(stderr, "read() error (body)\n");
        return -1;
    }

    // 3. Parse and pretty-print
    print_value(body.data(), body_len, 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./client <command> [args...]\n"
            "\n"
            "String commands:\n"
            "  ./client set  <key> <value>\n"
            "  ./client get  <key>\n"
            "  ./client del  <key>\n"
            "  ./client keys\n"
            "\n"
            "Sorted-set commands:\n"
            "  ./client zadd   <zset> <score> <name>\n"
            "  ./client zrem   <zset> <name>\n"
            "  ./client zscore <zset> <name>\n"
            "  ./client zquery <zset> <score> <name> <offset> <limit>\n"
        );
        return 1;
    }

    // Connect
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
        die("connect()");

    // Build command vector from argv
    std::vector<std::string> cmd;
    for (int i = 1; i < argc; i++) cmd.emplace_back(argv[i]);

    // Send → receive
    if (send_req(fd, cmd) != 0) {
        fprintf(stderr, "send failed\n");
        close(fd);
        return 1;
    }
    if (read_res(fd) != 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
