// SPDX-License-Identifier: MIT
// Platform byte order utilities.
// Portable replacement for compiler builtins like __builtin_bswap64.
#pragma once

#include <bit>
#include <cstdint>

namespace bsomeip::platform {

// Byte-swap a 64-bit value (host ↔ big-endian or vice versa).
// Uses std::byteswap (C++23) where available, falls back to compiler builtin.
constexpr std::uint64_t bswap64(std::uint64_t v) noexcept {
#if __cpp_lib_byteswap >= 202110L
    return std::byteswap(v);
#elif defined(__GNUC__) || defined(__clang__)
    if consteval {
        return ((v & 0x00000000000000FFull) << 56) |
               ((v & 0x000000000000FF00ull) << 40) |
               ((v & 0x0000000000FF0000ull) << 24) |
               ((v & 0x00000000FF000000ull) <<  8) |
               ((v & 0x000000FF00000000ull) >>  8) |
               ((v & 0x0000FF0000000000ull) >> 24) |
               ((v & 0x00FF000000000000ull) >> 40) |
               ((v & 0xFF00000000000000ull) >> 56);
    } else {
        return __builtin_bswap64(v);
    }
#elif defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) <<  8) |
           ((v & 0x000000FF00000000ull) >>  8) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
#endif
}

constexpr std::uint32_t bswap32(std::uint32_t v) noexcept {
#if __cpp_lib_byteswap >= 202110L
    return std::byteswap(v);
#elif defined(__GNUC__) || defined(__clang__)
    if consteval {
        return ((v & 0x000000FF) << 24) |
               ((v & 0x0000FF00) <<  8) |
               ((v & 0x00FF0000) >>  8) |
               ((v & 0xFF000000) >> 24);
    } else {
        return __builtin_bswap32(v);
    }
#elif defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return ((v & 0x000000FF) << 24) |
           ((v & 0x0000FF00) <<  8) |
           ((v & 0x00FF0000) >>  8) |
           ((v & 0xFF000000) >> 24);
#endif
}

} // namespace bsomeip::platform
