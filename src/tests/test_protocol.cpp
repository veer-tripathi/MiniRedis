// =============================================================================
// test_protocol.cpp — Day 2 parser tests
//
// Tests the three TCP realities the parser must handle:
//   1. Split packets  — message arrives byte by byte
//   2. Pipelining     — multiple complete messages in one recv()
//   3. Malformed data — truncated, oversized, or garbage input
//   4. Large payloads — near the k_max_msg limit
//
// Build:
//   g++ -std=c++17 -Isrc -o test_protocol \
//       src/tests/test_protocol.cpp \
//       src/protocol/protocol.cpp \
//       src/utils/buffer.cpp \
//       src/utils/logging.cpp
//
// Run:
//   ./test_protocol
// =============================================================================

#include "protocol/protocol.h"
#include "networking/connection.h"
#include "utils/buffer.h"
#include "protocol/serializer.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Mini test framework
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond) do { \
    if (cond) { ++g_pass; } \
    else { \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_fail; \
    } \
} while (0)

#define TEST(name) \
    static void name(); \
    struct _reg_##name { _reg_##name() { \
        printf("  %-50s", #name); \
        int before = g_fail; \
        name(); \
        puts(g_fail == before ? "OK" : "FAILED"); \
    }} _inst_##name; \
    static void name()

// ---------------------------------------------------------------------------
// Frame builder — creates a valid wire-format request frame
// ---------------------------------------------------------------------------

static std::vector<uint8_t> make_frame(const std::vector<std::string> &cmd) {
    // body = [nstrs : u32] [len : u32] [bytes] ...
    uint32_t body_len = 4;
    for (auto &s : cmd) body_len += 4 + (uint32_t)s.size();

    // Use the custom Buffer API to serialize safely
    Buffer frame = buf_init(body_len + 4);
    
    // outer frame header
    buf_append_u32(&frame, body_len);
    // body
    buf_append_u32(&frame, (uint32_t)cmd.size());
    for (auto &s : cmd) {
        buf_append_u32(&frame, (uint32_t)s.size());
        buf_append(&frame, (const uint8_t *)s.data(), s.size());
    }
    
    // Extract to std::vector for easy testing/memory management in macros
    std::vector<uint8_t> ret(frame.data_begin, frame.data_end);
    buf_free(&frame);
    return ret;
}

// A minimal try_one_request clone that only parses (no command dispatch) so
// tests don't need a running server.
static int parse_one(Conn &conn, std::vector<std::string> &out_cmd) {
    if (conn.incoming.size() < 4) return 0;   // need more data

    uint32_t body_len = 0;
    memcpy(&body_len, conn.incoming.data_begin, 4);

    static const size_t k_max_msg = 32 << 20;
    if (body_len > k_max_msg) return -1;      // too large

    if (4 + body_len > conn.incoming.size()) return 0;  // partial body

    const uint8_t *body = conn.incoming.data_begin + 4;
    if (parse_req(body, body_len, out_cmd) < 0) return -1;  // malformed

    buf_consume(&conn.incoming, 4 + body_len);
    return 1;   // success
}

// Drain all complete messages from conn into a vector of command vectors.
static std::vector<std::vector<std::string>> drain_all(Conn &conn) {
    std::vector<std::vector<std::string>> results;
    while (true) {
        std::vector<std::string> cmd;
        int rc = parse_one(conn, cmd);
        if (rc == 1) { results.push_back(cmd); continue; }
        break;
    }
    return results;
}

// ---------------------------------------------------------------------------
// 1. SPLIT PACKET TESTS
// ---------------------------------------------------------------------------

TEST(split_packet_byte_by_byte) {
    auto frame = make_frame({"set", "foo", "bar"});
    Conn conn;
    std::vector<std::string> cmd;

    for (size_t i = 0; i < frame.size() - 1; ++i) {
        buf_append_u8(&conn.incoming, frame[i]);
        int rc = parse_one(conn, cmd);
        EXPECT(rc == 0);           // not yet complete
        EXPECT(cmd.empty());       // nothing parsed
    }

    buf_append_u8(&conn.incoming, frame.back());
    int rc = parse_one(conn, cmd);
    EXPECT(rc == 1);
    EXPECT(cmd.size() == 3);
    EXPECT(cmd[0] == "set");
    EXPECT(cmd[1] == "foo");
    EXPECT(cmd[2] == "bar");
    EXPECT(conn.incoming.size() == 0);   // buffer fully consumed
}

TEST(split_packet_header_only) {
    auto frame = make_frame({"get", "mykey"});
    Conn conn;

    // Feed only the header (4 bytes).
    buf_append(&conn.incoming, frame.data(), 4);

    std::vector<std::string> cmd;
    int rc = parse_one(conn, cmd);
    EXPECT(rc == 0);       // partial — need body
    EXPECT(cmd.empty());

    // Now feed the rest.
    buf_append(&conn.incoming, frame.data() + 4, frame.size() - 4);
    rc = parse_one(conn, cmd);
    EXPECT(rc == 1);
    EXPECT(cmd.size() == 2);
    EXPECT(cmd[0] == "get");
    EXPECT(cmd[1] == "mykey");
}

TEST(split_packet_mid_body) {
    auto frame = make_frame({"zadd", "scores", "9.5", "alice"});
    Conn conn;
    size_t half = frame.size() / 2;

    // First half.
    buf_append(&conn.incoming, frame.data(), half);
    std::vector<std::string> cmd;
    EXPECT(parse_one(conn, cmd) == 0);
    EXPECT(cmd.empty());

    // Second half.
    buf_append(&conn.incoming, frame.data() + half, frame.size() - half);
    EXPECT(parse_one(conn, cmd) == 1);
    EXPECT(cmd.size() == 4);
    EXPECT(cmd[0] == "zadd");
    EXPECT(cmd[3] == "alice");
}

// ---------------------------------------------------------------------------
// 2. PIPELINING TESTS
// ---------------------------------------------------------------------------

TEST(pipeline_two_commands) {
    auto f1 = make_frame({"set", "k1", "v1"});
    auto f2 = make_frame({"set", "k2", "v2"});

    Conn conn;
    buf_append(&conn.incoming, f1.data(), f1.size());
    buf_append(&conn.incoming, f2.data(), f2.size());

    auto results = drain_all(conn);

    EXPECT(results.size() == 2);
    EXPECT(results[0][0] == "set" && results[0][1] == "k1");
    EXPECT(results[1][0] == "set" && results[1][1] == "k2");
    EXPECT(conn.incoming.size() == 0);
}

TEST(pipeline_ten_commands) {
    Conn conn;
    for (int i = 0; i < 10; ++i) {
        auto frame = make_frame({"get", "key" + std::to_string(i)});
        buf_append(&conn.incoming, frame.data(), frame.size());
    }

    auto results = drain_all(conn);
    EXPECT(results.size() == 10);
    for (int i = 0; i < 10; ++i) {
        EXPECT(results[i][0] == "get");
        EXPECT(results[i][1] == "key" + std::to_string(i));
    }
    EXPECT(conn.incoming.size() == 0);
}

TEST(pipeline_complete_then_partial) {
    auto f1 = make_frame({"del", "a"});
    auto f2 = make_frame({"del", "b"});
    auto f3 = make_frame({"del", "c"});

    Conn conn;
    buf_append(&conn.incoming, f1.data(), f1.size());
    buf_append(&conn.incoming, f2.data(), f2.size());
    buf_append(&conn.incoming, f3.data(), 3); // Only 3 bytes of f3

    auto results = drain_all(conn);
    EXPECT(results.size() == 2);       // only the two complete frames
    EXPECT(conn.incoming.size() == 3); // the partial header remains
}

// ---------------------------------------------------------------------------
// 3. MALFORMED INPUT TESTS
// ---------------------------------------------------------------------------

TEST(malformed_empty_body) {
    std::vector<std::string> cmd;
    int rc = parse_req(nullptr, 0, cmd);
    EXPECT(rc == -1);
}

TEST(malformed_nstrs_too_large) {
    uint8_t buf[4];
    uint32_t big = 999999;
    memcpy(buf, &big, 4);
    std::vector<std::string> cmd;
    int rc = parse_req(buf, sizeof(buf), cmd);
    EXPECT(rc == -1);
}

TEST(malformed_string_length_overflow) {
    uint8_t buf[8];
    uint32_t nstrs = 1;
    uint32_t slen  = 10000;   
    memcpy(buf,     &nstrs, 4);
    memcpy(buf + 4, &slen,  4);
    std::vector<std::string> cmd;
    int rc = parse_req(buf, sizeof(buf), cmd);
    EXPECT(rc == -1);
}

TEST(malformed_trailing_garbage) {
    auto frame = make_frame({"get", "key"});
    frame.push_back(0xFF);
    frame.push_back(0xDE);
    frame.push_back(0xAD);
    frame.push_back(0xBE);
    frame.push_back(0xEF);

    uint32_t new_body_len = 0;
    memcpy(&new_body_len, frame.data(), 4);
    new_body_len += 5;
    memcpy(frame.data(), &new_body_len, 4);

    std::vector<std::string> cmd;
    int rc = parse_req(frame.data() + 4, new_body_len, cmd);
    EXPECT(rc == -1);   // trailing garbage → error
}

TEST(malformed_frame_too_large) {
    Conn conn;
    uint32_t huge = 64 << 20;
    buf_append_u32(&conn.incoming, huge);

    std::vector<std::string> cmd;
    int rc = parse_one(conn, cmd);
    EXPECT(rc == -1);
}

// ---------------------------------------------------------------------------
// 4. LARGE PAYLOAD TESTS
// ---------------------------------------------------------------------------

TEST(large_payload_4KB_key) {
    std::string big_key(4096, 'A');
    auto frame = make_frame({"set", big_key, "val"});

    Conn conn;
    buf_append(&conn.incoming, frame.data(), frame.size());

    std::vector<std::string> cmd;
    int rc = parse_one(conn, cmd);
    EXPECT(rc == 1);
    EXPECT(cmd.size() == 3);
    EXPECT(cmd[1].size() == 4096);
    EXPECT(cmd[1] == big_key);
    EXPECT(conn.incoming.size() == 0);
}

TEST(large_payload_1MB_value) {
    std::string big_val(1 << 20, 'Z');   // 1 MB
    auto frame = make_frame({"set", "k", big_val});

    Conn conn;
    buf_append(&conn.incoming, frame.data(), frame.size());

    std::vector<std::string> cmd;
    int rc = parse_one(conn, cmd);
    EXPECT(rc == 1);
    EXPECT(cmd[2].size() == (size_t)(1 << 20));
    EXPECT(conn.incoming.size() == 0);
}

TEST(large_payload_many_small_args) {
    std::vector<std::string> cmd;
    cmd.push_back("multi");
    for (int i = 0; i < 999; ++i) cmd.push_back(std::string(32, 'x'));

    auto frame = make_frame(cmd);

    Conn conn;
    buf_append(&conn.incoming, frame.data(), frame.size());

    std::vector<std::string> out;
    int rc = parse_one(conn, out);
    EXPECT(rc == 1);
    EXPECT(out.size() == 1000);
}

// ---------------------------------------------------------------------------
// 5. BUFFER CURSOR TESTS
// ---------------------------------------------------------------------------

TEST(buffer_cursor_consume_no_copy) {
    Conn conn;
    for (int i = 0; i < 100; ++i) buf_append_u8(&conn.incoming, (uint8_t)i);
    
    EXPECT(conn.incoming.size() == 100);
    EXPECT(*conn.incoming.data_begin == 0);

    buf_consume(&conn.incoming, 10);
    EXPECT(conn.incoming.size() == 90);
    EXPECT(*conn.incoming.data_begin == 10);

    buf_consume(&conn.incoming, 90);
    EXPECT(conn.incoming.size() == 0);
}

TEST(buffer_cursor_compaction) {
    Conn conn;
    for (int i = 0; i < 5000; ++i) buf_append_u8(&conn.incoming, (uint8_t)(i & 0xFF));
    EXPECT(conn.incoming.size() == 5000);

    buf_consume(&conn.incoming, 5000);

    // Buffer fully consumed -> data_begin should reset to buffer_begin
    EXPECT(conn.incoming.size() == 0);
    EXPECT(conn.incoming.data_begin == conn.incoming.buffer_begin); 

    // Small consumes when NOT empty do NOT reset the cursor
    buf_append_u8(&conn.incoming, 0xAA);
    buf_append_u8(&conn.incoming, 0xBB);
    buf_consume(&conn.incoming, 1);
    
    EXPECT(conn.incoming.data_begin > conn.incoming.buffer_begin);
    EXPECT(conn.incoming.size() == 1);     // 1 byte (0xBB) still pending
}

TEST(buffer_outgoing_cursor) {
    Conn conn;
    for (int i = 0; i < 50; ++i) buf_append_u8(&conn.outgoing, (uint8_t)i);
    EXPECT(conn.outgoing.size() == 50);
    EXPECT(*conn.outgoing.data_begin == 0);

    buf_consume(&conn.outgoing, 20);
    EXPECT(conn.outgoing.size() == 30);
    EXPECT(*conn.outgoing.data_begin == 20);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("\n=== Day 2 — Incremental Protocol Parser Tests ===\n\n");
    printf("  --- Split packet ---\n");

    printf("\n  Results: %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}