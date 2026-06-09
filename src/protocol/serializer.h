#pragma once

#include "../utils/buffer.h"
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Response status codes (carried in the 4-byte message header)
// ---------------------------------------------------------------------------
enum {
    RES_OK  = 0,
    RES_ERR = 1,
    RES_NX  = 2,
};

// ---------------------------------------------------------------------------
// Application-level error codes  (inside a TAG_ERR payload)
// ---------------------------------------------------------------------------
enum {
    ERR_UNKNOWN  = 1,
    ERR_TOO_BIG  = 2,
    ERR_BAD_TYPE = 3,
    ERR_BAD_ARG  = 4,
};

// ---------------------------------------------------------------------------
// Serialised data-type tags (first byte of every value)
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
// Typed value serialisers
// ---------------------------------------------------------------------------
void out_nil(Buffer *out);
void out_str(Buffer *out, const char *s, size_t size);
void out_int(Buffer *out, int64_t n);
void out_dbl(Buffer *out, double  n);
void out_err(Buffer *out, uint32_t code, const std::string &s);
void out_arr(Buffer *out, uint32_t n);

// ---------------------------------------------------------------------------
// Response framing helpers
// ---------------------------------------------------------------------------

// Reserve a 4-byte length header and record its position in `*header`.
void   response_begin(Buffer *out, size_t *header);

// Number of payload bytes written after the header.
size_t response_size (Buffer *out, size_t  header);

// Fill in the reserved header (and truncate + emit ERR_TOO_BIG if needed).
void   response_end  (Buffer *out, size_t  header);