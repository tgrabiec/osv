#ifndef _HASHING_HH
#define _HASHING_HH

#include <stdint.h>
#include <stddef.h>


/**
 * Collection of decent hashing function.
 *
 * Note: both std::hash and boost::hash do not produce good hashes for primitives.
 *
 * Source: http://smhasher.googlecode.com/svn/trunk/MurmurHash3.cpp
 * 
 * Disclaimer form the source:
 * "MurmurHash3 was written by Austin Appleby, and is placed in the public
 *  domain. The author hereby disclaims copyright to this source code."
 * 
 */

__attribute__((unused))
static uint64_t hash64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (long) h;
}

__attribute__((unused))
static uint32_t hash32(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

template<typename T>
static size_t hash(T x);

template<>
size_t hash<size_t>(size_t x) {
  return (size_t) hash64((uint64_t) x);
}

template<>
size_t hash<void*>(void* x) {
  return (size_t) hash64((uint64_t) x);
}

//     static_assert(sizeof(void*) == 8, "type");
//     static_assert(sizeof(x) <= 8, "type is too long");
//     static_assert(sizeof(size_t) == 8 || sizeof(size_t) == 4, "unsupported sizeof(size_t)");

//     if (sizeof(size_t) == 4 && sizeof(T) <= 4)
//             return (size_t) hash32((uint32_t) (x));

//     return (size_t) hash64((uint64_t) (x));
// }

#endif
