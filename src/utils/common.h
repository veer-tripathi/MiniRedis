#pragma once

#include <stdint.h>
#include <stddef.h>

// Intrusive container helper: recover the enclosing struct from a member pointer.
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// FNV-1a 64-bit hash for hashmap keys.
inline uint64_t str_hash(const uint8_t *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}
