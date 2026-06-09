#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Maximum number of arguments in a single request.
static const size_t k_max_args = 200'000;

// Parse a request body of `size` bytes starting at `data`.
//
// Wire format:
//   +-------+-----+------+-----+------+-----+-----+------+
//   | nstrs | len | str1 | len | str2 | ... | len | strN |
//   +-------+-----+------+-----+------+-----+-----+------+
// All length fields are little-endian uint32_t.
//
// Returns 0 on success, -1 on malformed input.
int32_t parse_req(const uint8_t *data, size_t size,
                  std::vector<std::string> &out);
