#include "protocol.h"

#include <cstring>  // memcpy

// ---------------------------------------------------------------------------
// Internal reading helpers (advance a cursor through a raw byte span)
// ---------------------------------------------------------------------------

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end,
                     size_t n, std::string &out) {
    if (cur + n > end) return false;
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int32_t parse_req(const uint8_t *data, size_t size,
                  std::vector<std::string> &out) {
    const uint8_t *end = data + size;

    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr))  return -1;
    if (nstr > k_max_args)           return -1;

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) return -1;
        out.emplace_back();
        if (!read_str(data, end, len, out.back())) return -1;
    }

    if (data != end) return -1;   // trailing garbage
    return 0;
}
