// learning/ch11/common.h
//
// Two small helpers that both the server and the sorted set need, so
// they live in one shared header (chapters 8-10 kept copies in each file).

#pragma once

#include <stddef.h>
#include <stdint.h>

// From a pointer to a member embedded inside a struct, compute the
// pointer to the whole struct. (Explained in chapter 8.)
//
//   memory:  [ struct start | ... | member | ... ]
//                                   ^ ptr points here
//   subtract offsetof(type, member) to get back to the struct start.
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// FNV-1a string hash: for each byte, XOR it in, then multiply by a prime.
inline uint64_t str_hash(const uint8_t *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;   // FNV offset basis
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;              // FNV prime
    }
    return h;
}
