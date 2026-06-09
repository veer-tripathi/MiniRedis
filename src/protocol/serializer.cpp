#include "serializer.h"
#include "../utils/buffer.h"

#include <cstring>   // memcpy
#include <string>

// Maximum single response size — matches the socket read buffer in networking.
static const size_t k_max_msg = 32 << 20;

// ---------------------------------------------------------------------------
// Typed value serialisers
// ---------------------------------------------------------------------------

void out_nil(Buffer *out) {
    buf_append_u8(out, TAG_NIL);
}

void out_str(Buffer *out, const char *s, size_t size) {
    buf_append_u8 (out, TAG_STR);
    buf_append_u32(out, (uint32_t)size);
    buf_append    (out, (const uint8_t *)s, size);
}

void out_int(Buffer *out, int64_t n) {
    buf_append_u8 (out, TAG_INT);
    buf_append_i64(out, n);
}

void out_dbl(Buffer *out, double n) {
    buf_append_u8 (out, TAG_DBL);
    buf_append_dbl(out, n);
}

void out_err(Buffer *out, uint32_t code, const std::string &s) {
    buf_append_u8 (out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)s.size());
    buf_append    (out, (const uint8_t *)s.data(), s.size());
}

void out_arr(Buffer *out, uint32_t n) {
    buf_append_u8 (out, TAG_ARR);
    buf_append_u32(out, n);
}

// ---------------------------------------------------------------------------
// Response framing
// ---------------------------------------------------------------------------

void response_begin(Buffer *out, size_t *header) {
    *header = out->size();
    buf_append_u32(out, 0);   // placeholder length
}

size_t response_size(Buffer *out, size_t header) {
    return out->size() - header - 4;
}

void response_end(Buffer *out, size_t header) {
    size_t msg_size = response_size(out, header);
    
    if (msg_size > k_max_msg) {
        // Equivalent to out.resize(header + 4): 
        // Move the end pointer back to the end of the header placeholder
        out->data_end = out->data_begin + header + 4;
        
        out_err(out, ERR_TOO_BIG, "response is too big");
        msg_size = response_size(out, header);
    }
    
    uint32_t len = (uint32_t)msg_size;
    
    // Equivalent to &out[header]:
    // Safely write the length into the placeholder space
    memcpy(out->data_begin + header, &len, 4);
}