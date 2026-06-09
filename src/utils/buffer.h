#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

struct Buffer {
    uint8_t *buffer_begin = nullptr;  // start of allocated memory
    uint8_t *buffer_end   = nullptr;  // one past end of allocation
    uint8_t *data_begin   = nullptr;  // first unread byte
    uint8_t *data_end     = nullptr;  // one past last written byte

    // How many bytes of valid data are in the buffer
    size_t size()     const { return (size_t)(data_end   - data_begin);   }
    // Total allocated memory
    size_t capacity() const { return (size_t)(buffer_end - buffer_begin); }
    // Free space at the back
    size_t free_back()  const { return (size_t)(buffer_end - data_end);   }
    // Free space at the front (already consumed, reusable)
    size_t free_front() const { return (size_t)(data_begin - buffer_begin); }

    // Raw pointer to first valid byte — safe for parse_req() and write()
    const uint8_t *data() const { return data_begin; }
          uint8_t *data()       { return data_begin; }
};

// Allocate a new buffer with given initial capacity.
Buffer buf_init(size_t initial_capacity = 4096);

// Free the underlying allocation.
void buf_free(Buffer *buf);

// O(1) — just advance the front pointer. No data is moved.
void buf_consume(Buffer *buf, size_t n);

// Amortized O(1) — tries to use slack space before reallocating.
void buf_append(Buffer *buf, const uint8_t *data, size_t len);

// Typed append helpers (used by serializer)
void buf_append_u8 (Buffer *buf, uint8_t  v);
void buf_append_u32(Buffer *buf, uint32_t v);
void buf_append_i64(Buffer *buf, int64_t  v);
void buf_append_dbl(Buffer *buf, double   v);