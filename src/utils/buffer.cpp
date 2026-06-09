#include "buffer.h"

Buffer buf_init(size_t initial_capacity) {
    uint8_t *mem = (uint8_t *)malloc(initial_capacity);
    return { mem, mem + initial_capacity, mem, mem };
}

void buf_free(Buffer *buf) {
    free(buf->buffer_begin);
    *buf = {};
}

void buf_consume(Buffer *buf, size_t n) {
    buf->data_begin += n;
    // If buffer is now empty, reset both pointers to the front.
    // This avoids the "slide to the left" step in many common cases.
    if (buf->data_begin == buf->data_end) {
        buf->data_begin = buf->buffer_begin;
        buf->data_end   = buf->buffer_begin;
    }
}

void buf_append(Buffer *buf, const uint8_t *data, size_t len) {
    // Case 1: enough space at the back — just write there.
    if (buf->free_back() >= len) {
        memcpy(buf->data_end, data, len);
        buf->data_end += len;
        return;
    }

    // Case 2: not enough at the back, but total free space is enough.
    // Slide existing data to the front of the allocation (memmove),
    // then append. This recovers the "wasted" front space.
    size_t used = buf->size();
    if (used + len <= buf->capacity()) {
        memmove(buf->buffer_begin, buf->data_begin, used);
        buf->data_begin = buf->buffer_begin;
        buf->data_end   = buf->buffer_begin + used;
        memcpy(buf->data_end, data, len);
        buf->data_end += len;
        return;
    }

    // Case 3: not enough total space — reallocate.
    // Double the capacity, or grow just enough, whichever is larger.
    size_t new_cap = std::max(buf->capacity() * 2, used + len);
    uint8_t *mem = (uint8_t *)malloc(new_cap);
    memcpy(mem, buf->data_begin, used);
    free(buf->buffer_begin);
    buf->buffer_begin = mem;
    buf->buffer_end   = mem + new_cap;
    buf->data_begin   = mem;
    buf->data_end     = mem + used;
    // Now case 1 always applies.
    memcpy(buf->data_end, data, len);
    buf->data_end += len;
}

void buf_append_u8 (Buffer *buf, uint8_t  v) { buf_append(buf, &v, 1); }
void buf_append_u32(Buffer *buf, uint32_t v) { buf_append(buf, (uint8_t *)&v, 4); }
void buf_append_i64(Buffer *buf, int64_t  v) { buf_append(buf, (uint8_t *)&v, 8); }
void buf_append_dbl(Buffer *buf, double   v) { buf_append(buf, (uint8_t *)&v, 8); }