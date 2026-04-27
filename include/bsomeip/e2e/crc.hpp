// SPDX-License-Identifier: MIT
// E2E CRC computation: constexpr CRC-8/SAE-J1850, CRC-32/Ethernet, CRC-64.
// Used by AUTOSAR E2E profiles P01, P02, P04, P07.
//
// Runtime acceleration: slicing-by-8 for CRC-32 and CRC-64.
// Processes 8 bytes per iteration (~5-8x faster than byte-at-a-time).
// Compile-time: falls back to byte-at-a-time via `if consteval`.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <bsomeip/platform/byte_order.hpp>

namespace bsomeip::e2e {

// ============================================================
// CRC-8/SAE-J1850 (polynomial 0x1D, init 0xFF, xorout 0xFF)
// Used by E2E Profile 01 and Profile 02.
// ============================================================

namespace detail {

consteval std::array<std::uint8_t, 256> make_crc8_table() noexcept {
    constexpr std::uint8_t poly = 0x1D;
    std::array<std::uint8_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        std::uint8_t crc = static_cast<std::uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80)
                crc = static_cast<std::uint8_t>((crc << 1) ^ poly);
            else
                crc = static_cast<std::uint8_t>(crc << 1);
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto crc8_table = make_crc8_table();

} // namespace detail

constexpr std::uint8_t crc8_sae_j1850(std::span<const std::byte> data,
                                       std::uint8_t init = 0xFF) noexcept {
    std::uint8_t crc = init;
    for (auto b : data) {
        crc = detail::crc8_table[crc ^ static_cast<std::uint8_t>(b)];
    }
    return crc ^ 0xFF;
}

// ============================================================
// CRC-32/Ethernet (polynomial 0x04C11DB7, reflected, init 0xFFFFFFFF)
// Used by E2E Profile 04.
//
// Runtime: slicing-by-8 — 8 tables × 256 entries = 8KB .rodata.
// ============================================================

namespace detail {

consteval std::array<std::uint32_t, 256> make_crc32_table() noexcept {
    constexpr std::uint32_t poly = 0xEDB88320u; // reflected 0x04C11DB7
    std::array<std::uint32_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1)
                crc = (crc >> 1) ^ poly;
            else
                crc >>= 1;
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto crc32_table = make_crc32_table();

// Slicing-by-8 tables for CRC-32/Ethernet.
// T[0] = standard table. T[k][i] = CRC of byte i followed by k zero bytes.
struct crc32_slice8 {
    std::array<std::array<std::uint32_t, 256>, 8> t;
};

consteval crc32_slice8 make_crc32_slice8() noexcept {
    crc32_slice8 result{};
    result.t[0] = make_crc32_table();
    for (unsigned k = 1; k < 8; ++k) {
        for (unsigned i = 0; i < 256; ++i) {
            result.t[k][i] = (result.t[k-1][i] >> 8)
                            ^ result.t[0][result.t[k-1][i] & 0xFF];
        }
    }
    return result;
}

inline constexpr auto crc32_s8 = make_crc32_slice8();

inline std::uint32_t crc32_fast(const std::byte* data, std::size_t len,
                                std::uint32_t crc) noexcept {
    auto& T = crc32_s8.t;
    auto p = reinterpret_cast<const std::uint8_t*>(data);

    // Process 8 bytes at a time
    while (len >= 8) {
        std::uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = T[7][(a      ) & 0xFF] ^
              T[6][(a >>  8) & 0xFF] ^
              T[5][(a >> 16) & 0xFF] ^
              T[4][(a >> 24)       ] ^
              T[3][(b      ) & 0xFF] ^
              T[2][(b >>  8) & 0xFF] ^
              T[1][(b >> 16) & 0xFF] ^
              T[0][(b >> 24)       ];
        p += 8;
        len -= 8;
    }

    // Remainder: byte-at-a-time
    while (len--) {
        crc = T[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

} // namespace detail

constexpr std::uint32_t crc32_ethernet(std::span<const std::byte> data,
                                        std::uint32_t init = 0xFFFFFFFF) noexcept {
    if consteval {
        std::uint32_t crc = init;
        for (auto b : data) {
            crc = detail::crc32_table[(crc ^ static_cast<std::uint8_t>(b)) & 0xFF]
                  ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFF;
    } else {
        return detail::crc32_fast(data.data(), data.size(), init);
    }
}

// ============================================================
// CRC-64 (polynomial 0x42F0E1EBA9EA3693, init 0xFFFFFFFFFFFFFFFF)
// Used by E2E Profile 07.
//
// Non-reflected (MSB-first). Runtime: slicing-by-8, 16KB .rodata.
// ============================================================

namespace detail {

consteval std::array<std::uint64_t, 256> make_crc64_table() noexcept {
    constexpr std::uint64_t poly = 0x42F0E1EBA9EA3693ull;
    std::array<std::uint64_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        std::uint64_t crc = static_cast<std::uint64_t>(i) << 56;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & (1ull << 63))
                crc = (crc << 1) ^ poly;
            else
                crc <<= 1;
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto crc64_table = make_crc64_table();

// Slicing-by-8 tables for CRC-64 (non-reflected).
// T[k][i] = CRC contribution of byte i at position k (0=rightmost).
struct crc64_slice8 {
    std::array<std::array<std::uint64_t, 256>, 8> t;
};

consteval crc64_slice8 make_crc64_slice8() noexcept {
    crc64_slice8 result{};
    result.t[0] = make_crc64_table();
    for (unsigned k = 1; k < 8; ++k) {
        for (unsigned i = 0; i < 256; ++i) {
            result.t[k][i] = (result.t[k-1][i] << 8)
                            ^ result.t[0][static_cast<std::uint8_t>(result.t[k-1][i] >> 56)];
        }
    }
    return result;
}

inline constexpr auto crc64_s8 = make_crc64_slice8();

inline std::uint64_t crc64_fast(const std::byte* data, std::size_t len,
                                std::uint64_t crc) noexcept {
    auto& T = crc64_s8.t;
    auto p = reinterpret_cast<const std::uint8_t*>(data);

    // Process 8 bytes at a time.
    // Non-reflected: XOR input with MSB of CRC, need big-endian byte order.
    while (len >= 8) {
        // Byte-swap the 8 input bytes to big-endian for XOR with CRC (MSB-first)
        std::uint64_t block;
        std::memcpy(&block, p, 8);
        block = platform::bswap64(block);
        std::uint64_t combined = crc ^ block;
        crc = T[7][static_cast<std::uint8_t>(combined >> 56)] ^
              T[6][static_cast<std::uint8_t>(combined >> 48)] ^
              T[5][static_cast<std::uint8_t>(combined >> 40)] ^
              T[4][static_cast<std::uint8_t>(combined >> 32)] ^
              T[3][static_cast<std::uint8_t>(combined >> 24)] ^
              T[2][static_cast<std::uint8_t>(combined >> 16)] ^
              T[1][static_cast<std::uint8_t>(combined >>  8)] ^
              T[0][static_cast<std::uint8_t>(combined      )];
        p += 8;
        len -= 8;
    }

    // Remainder: byte-at-a-time
    while (len--) {
        auto idx = static_cast<std::uint8_t>((crc >> 56) ^ *p++);
        crc = T[0][idx] ^ (crc << 8);
    }

    return crc ^ 0xFFFFFFFFFFFFFFFFull;
}

} // namespace detail

constexpr std::uint64_t crc64(std::span<const std::byte> data,
                               std::uint64_t init = 0xFFFFFFFFFFFFFFFFull) noexcept {
    if consteval {
        std::uint64_t crc = init;
        for (auto b : data) {
            auto idx = static_cast<std::uint8_t>((crc >> 56) ^ static_cast<std::uint8_t>(b));
            crc = detail::crc64_table[idx] ^ (crc << 8);
        }
        return crc ^ 0xFFFFFFFFFFFFFFFFull;
    } else {
        return detail::crc64_fast(data.data(), data.size(), init);
    }
}

} // namespace bsomeip::e2e
