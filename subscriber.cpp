// =============================================================================
// subscriber.cpp — stays connected after SUBSCRIBE and prints pushed messages.
//
// Build:   g++ -std=c++17 -o subscriber subscriber.cpp
// Usage:   ./subscriber <channel> [output_file]
//
// If output_file is given, each received message is appended there (one per
// line) in addition to stdout. The test script uses this to check delivery.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <string>
#include <vector>

static const size_t k_max_msg = 32 << 20;

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

static void die(const char *msg) {
    fprintf(stderr, "[errno %d] %s\n", errno, msg);
    abort();
}

static int32_t read_full(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
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

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t body_len = 4;
    for (const std::string &s : cmd)
        body_len += 4 + (uint32_t)s.size();

    std::vector<uint8_t> wbuf(4 + body_len);
    memcpy(wbuf.data(), &body_len, 4);
    uint32_t nstrs = (uint32_t)cmd.size();
    memcpy(wbuf.data() + 4, &nstrs, 4);

    size_t cur = 8;
    for (const std::string &s : cmd) {
        uint32_t slen = (uint32_t)s.size();
        memcpy(wbuf.data() + cur, &slen, 4);
        memcpy(wbuf.data() + cur + 4, s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf.data(), wbuf.size());
}

// Parse a string value out of a buffer at pos.
// Returns the string, advances pos.
static std::string parse_str(const uint8_t *buf, uint32_t len, uint32_t &pos) {
    if (pos + 4 > len) return "";
    uint32_t slen = 0;
    memcpy(&slen, buf + pos, 4);
    pos += 4;
    if (pos + slen > len) return "";
    std::string s((const char *)buf + pos, slen);
    pos += slen;
    return s;
}

// Parse one response frame.
// For pub/sub arrays: extract and return the message body.
// For subscribe confirmations: print and return empty string.
// Returns "" for non-message frames, message body for message frames.
static std::string parse_frame(const uint8_t *buf, uint32_t len) {
    if (len == 0) return "";
    uint8_t tag = buf[0];

    if (tag != TAG_ARR) return "";

    uint32_t pos = 1;
    if (pos + 4 > len) return "";
    uint32_t n = 0;
    memcpy(&n, buf + pos, 4);
    pos += 4;

    if (n != 3) return "";

    // Element 0: kind string (subscribe / unsubscribe / message)
    if (pos >= len || buf[pos] != TAG_STR) return "";
    pos++;
    std::string kind = parse_str(buf, len, pos);

    // Element 1: channel
    if (pos >= len || buf[pos] != TAG_STR) return "";
    pos++;
    std::string channel = parse_str(buf, len, pos);

    if (kind == "subscribe") {
        // Element 2: subscription count (int)
        if (pos + 9 > len) return "";
        int64_t count = 0;
        memcpy(&count, buf + pos + 1, 8);
        fprintf(stdout, "[subscribed] channel=%s count=%ld\n",
                channel.c_str(), count);
        fflush(stdout);
        return "";
    }

    if (kind == "message") {
        // Element 2: message body (string)
        if (pos >= len || buf[pos] != TAG_STR) return "";
        pos++;
        std::string message = parse_str(buf, len, pos);
        fprintf(stdout, "[message] channel=%s msg=%s\n",
                channel.c_str(), message.c_str());
        fflush(stdout);
        return message;
    }

    return "";
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./subscriber <channel> [output_file]\n");
        return 1;
    }

    const char *channel     = argv[1];
    const char *output_file = argc >= 3 ? argv[2] : nullptr;

    // Connect
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
        die("connect()");

    // Send SUBSCRIBE
    std::vector<std::string> cmd = {"subscribe", channel};
    if (send_req(fd, cmd) != 0) die("send_req()");

    // Open output file if requested
    FILE *outf = nullptr;
    if (output_file) {
        outf = fopen(output_file, "w");
        if (!outf) die("fopen()");
    }

    // Read loop — runs until the connection is closed
    while (true) {
        uint8_t lenbuf[4];
        if (read_full(fd, lenbuf, 4) != 0) break;

        uint32_t body_len = 0;
        memcpy(&body_len, lenbuf, 4);
        if (body_len > k_max_msg) break;

        std::vector<uint8_t> body(body_len);
        if (read_full(fd, body.data(), body_len) != 0) break;

        std::string msg = parse_frame(body.data(), body_len);

        // Write to output file so the test script can check it
        if (outf && !msg.empty()) {
            fprintf(outf, "%s\n", msg.c_str());
            fflush(outf);
        }
    }

    if (outf) fclose(outf);
    close(fd);
    return 0;
}