// SPDX-License-Identifier: MIT
// E2E protection profiles: P01, P02, P04, P07.
// Each profile defines how to compute/verify the CRC over message data,
// including counter management and data-ID handling.
//
// AUTOSAR E2E specification reference:
//   - Profile 01: CRC-8/SAE-J1850, 4-bit counter, 16-bit data ID
//   - Profile 02: CRC-8/SAE-J1850, 4-bit counter, 8-bit data ID list
//   - Profile 04: CRC-32/Ethernet, 16-bit counter, 32-bit data ID, 16-bit length
//   - Profile 07: CRC-64, 32-bit counter, 32-bit data ID, 32-bit length
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <vector>

#include <bsomeip/e2e/crc.hpp>

namespace bsomeip::e2e {

// E2E check result
enum class e2e_result : std::uint8_t {
    ok,                 // CRC and counter valid
    wrong_crc,          // CRC mismatch
    wrong_sequence,     // Counter not in expected range
    repeated,           // Same counter as last (duplicate)
    no_new_data,        // No data received yet
    error,              // General error (too short, etc.)
};

// ============================================================
// Profile 01: CRC-8, 4-bit counter, 16-bit data ID
//
// Header layout in protected payload:
//   Byte 0:  CRC (8 bit)
//   Byte 1:  [counter:4 | data_id_nibble:4] (nibble layout depends on config)
//
// The CRC is computed over:
//   - data_id (2 bytes, high then low)
//   - payload bytes (excluding the CRC byte itself)
// ============================================================

struct profile_01_config {
    std::uint16_t data_id{0};
    std::uint16_t crc_offset{0};        // byte offset of CRC in payload
    std::uint16_t counter_offset{1};    // byte offset of counter nibble
    std::uint16_t data_id_mode{0};      // 0=BOTH, 1=LOW, 2=ALT
    std::uint16_t max_delta_counter{1}; // max allowed counter jump
};

struct profile_01 {
    using config_type = profile_01_config;

    // Write CRC and counter into payload.
    // payload must contain the E2E-protected data area.
    static void protect(std::span<std::byte> payload,
                        const profile_01_config& cfg,
                        std::uint8_t counter) noexcept {
        if (payload.size() < 2) return;

        // Write counter (low nibble of counter_offset byte)
        auto& counter_byte = payload[cfg.counter_offset];
        counter_byte = static_cast<std::byte>(
            (static_cast<std::uint8_t>(counter_byte) & 0xF0) | (counter & 0x0F));

        // Clear CRC byte before computation
        payload[cfg.crc_offset] = std::byte{0};

        // Compute CRC over data_id bytes + payload (excluding CRC position)
        std::uint8_t crc = 0xFF;
        // Feed data_id high byte, then low byte
        std::array<std::byte, 2> id_bytes{
            static_cast<std::byte>(cfg.data_id >> 8),
            static_cast<std::byte>(cfg.data_id & 0xFF)};
        crc = crc8_sae_j1850(id_bytes, 0xFF);

        // Feed all payload bytes except CRC position
        for (std::size_t i = 0; i < payload.size(); ++i) {
            if (i == cfg.crc_offset) continue;
            std::array<std::byte, 1> b{payload[i]};
            // Continue CRC from previous state (un-finalize then feed)
            crc = crc8_sae_j1850(b, crc ^ 0xFF);
        }
        payload[cfg.crc_offset] = static_cast<std::byte>(crc);
    }

    // Verify CRC and check counter.
    static e2e_result check(std::span<const std::byte> payload,
                            const profile_01_config& cfg,
                            std::uint8_t& last_counter) noexcept {
        if (payload.size() < 2) return e2e_result::error;

        // Extract received counter
        std::uint8_t recv_counter =
            static_cast<std::uint8_t>(payload[cfg.counter_offset]) & 0x0F;

        // Recompute CRC
        std::array<std::byte, 2> id_bytes{
            static_cast<std::byte>(cfg.data_id >> 8),
            static_cast<std::byte>(cfg.data_id & 0xFF)};
        std::uint8_t crc = crc8_sae_j1850(id_bytes, 0xFF);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            if (i == cfg.crc_offset) continue;
            std::array<std::byte, 1> b{payload[i]};
            crc = crc8_sae_j1850(b, crc ^ 0xFF);
        }

        std::uint8_t recv_crc = static_cast<std::uint8_t>(payload[cfg.crc_offset]);
        if (crc != recv_crc) return e2e_result::wrong_crc;

        // Counter check
        std::uint8_t delta = (recv_counter - last_counter) & 0x0F;
        e2e_result result;
        if (delta == 0) {
            result = e2e_result::repeated;
        } else if (delta <= cfg.max_delta_counter) {
            result = e2e_result::ok;
        } else {
            result = e2e_result::wrong_sequence;
        }
        last_counter = recv_counter;
        return result;
    }
};

// ============================================================
// Profile 02: CRC-8, 4-bit counter, 8-bit data ID per nibble
//
// Header layout:
//   Byte 0: CRC (8 bit)
//   Byte 1: [counter:4 | data_id_nibble:4]
// ============================================================

struct profile_02_config {
    std::uint8_t data_id_list[16]{};  // maps counter nibble → data_id byte
    std::uint16_t max_delta_counter{1};
};

struct profile_02 {
    using config_type = profile_02_config;

    static void protect(std::span<std::byte> payload,
                        const profile_02_config& cfg,
                        std::uint8_t counter) noexcept {
        if (payload.size() < 2) return;

        // Write counter (low nibble of byte 1)
        payload[1] = static_cast<std::byte>(
            (static_cast<std::uint8_t>(payload[1]) & 0xF0) | (counter & 0x0F));

        payload[0] = std::byte{0};

        // Data ID for this counter value
        std::uint8_t data_id = cfg.data_id_list[counter & 0x0F];

        // CRC over data_id + payload bytes (skip CRC position=0)
        std::array<std::byte, 1> id_byte{static_cast<std::byte>(data_id)};
        std::uint8_t crc = crc8_sae_j1850(id_byte, 0xFF);
        for (std::size_t i = 1; i < payload.size(); ++i) {
            std::array<std::byte, 1> b{payload[i]};
            crc = crc8_sae_j1850(b, crc ^ 0xFF);
        }
        payload[0] = static_cast<std::byte>(crc);
    }

    static e2e_result check(std::span<const std::byte> payload,
                            const profile_02_config& cfg,
                            std::uint8_t& last_counter) noexcept {
        if (payload.size() < 2) return e2e_result::error;

        std::uint8_t recv_counter =
            static_cast<std::uint8_t>(payload[1]) & 0x0F;
        std::uint8_t data_id = cfg.data_id_list[recv_counter & 0x0F];

        std::array<std::byte, 1> id_byte{static_cast<std::byte>(data_id)};
        std::uint8_t crc = crc8_sae_j1850(id_byte, 0xFF);
        for (std::size_t i = 1; i < payload.size(); ++i) {
            std::array<std::byte, 1> b{payload[i]};
            crc = crc8_sae_j1850(b, crc ^ 0xFF);
        }

        if (crc != static_cast<std::uint8_t>(payload[0]))
            return e2e_result::wrong_crc;

        std::uint8_t delta = (recv_counter - last_counter) & 0x0F;
        e2e_result result;
        if (delta == 0)
            result = e2e_result::repeated;
        else if (delta <= cfg.max_delta_counter)
            result = e2e_result::ok;
        else
            result = e2e_result::wrong_sequence;
        last_counter = recv_counter;
        return result;
    }
};

// ============================================================
// Profile 04: CRC-32, 16-bit counter, 32-bit data ID, 16-bit length
//
// Header layout (12 bytes prepended to data):
//   Bytes  0-1:  Length (16 bit, big-endian)
//   Bytes  2-3:  Counter (16 bit, big-endian)
//   Bytes  4-7:  Data ID (32 bit, big-endian)
//   Bytes  8-11: CRC-32 (32 bit, big-endian)
// ============================================================

struct profile_04_config {
    std::uint32_t data_id{0};
    std::uint16_t min_data_length{12};   // minimum protected area length
    std::uint16_t max_data_length{4096};
    std::uint16_t max_delta_counter{1};
};

struct profile_04 {
    using config_type = profile_04_config;
    static constexpr std::size_t header_size = 12;

    static void write_u16_be(std::byte* p, std::uint16_t v) noexcept {
        p[0] = static_cast<std::byte>(v >> 8);
        p[1] = static_cast<std::byte>(v);
    }
    static void write_u32_be(std::byte* p, std::uint32_t v) noexcept {
        p[0] = static_cast<std::byte>(v >> 24);
        p[1] = static_cast<std::byte>(v >> 16);
        p[2] = static_cast<std::byte>(v >> 8);
        p[3] = static_cast<std::byte>(v);
    }
    static std::uint16_t read_u16_be(const std::byte* p) noexcept {
        return static_cast<std::uint16_t>(
            (static_cast<unsigned>(p[0]) << 8) | static_cast<unsigned>(p[1]));
    }
    static std::uint32_t read_u32_be(const std::byte* p) noexcept {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8)  |
                static_cast<std::uint32_t>(p[3]);
    }

    static void protect(std::span<std::byte> payload,
                        const profile_04_config& cfg,
                        std::uint16_t counter) noexcept {
        if (payload.size() < header_size) return;

        // Write header fields
        write_u16_be(payload.data() + 0, static_cast<std::uint16_t>(payload.size()));
        write_u16_be(payload.data() + 2, counter);
        write_u32_be(payload.data() + 4, cfg.data_id);

        // Zero CRC field before computation
        write_u32_be(payload.data() + 8, 0);

        // CRC-32 over entire payload
        std::uint32_t crc = crc32_ethernet(payload);
        write_u32_be(payload.data() + 8, crc);
    }

    static e2e_result check(std::span<const std::byte> payload,
                            const profile_04_config& cfg,
                            std::uint16_t& last_counter) noexcept {
        if (payload.size() < header_size) return e2e_result::error;

        std::uint16_t recv_len = read_u16_be(payload.data());
        if (recv_len != payload.size()) return e2e_result::error;

        std::uint16_t recv_counter = read_u16_be(payload.data() + 2);
        std::uint32_t recv_data_id = read_u32_be(payload.data() + 4);
        if (recv_data_id != cfg.data_id) return e2e_result::wrong_crc;

        std::uint32_t recv_crc = read_u32_be(payload.data() + 8);

        // Recompute CRC with CRC field zeroed, using fast slicing-by-8.
        // Stack-allocate a copy for typical E2E payloads.
        auto buf = std::vector<std::byte>(payload.begin(), payload.end());
        std::memset(buf.data() + 8, 0, 4);
        std::uint32_t crc = crc32_ethernet(std::span<const std::byte>{buf});

        if (crc != recv_crc) return e2e_result::wrong_crc;

        // Counter check (16-bit wrapping)
        std::uint16_t delta = static_cast<std::uint16_t>(recv_counter - last_counter);
        e2e_result result;
        if (delta == 0)
            result = e2e_result::repeated;
        else if (delta <= cfg.max_delta_counter)
            result = e2e_result::ok;
        else
            result = e2e_result::wrong_sequence;
        last_counter = recv_counter;
        return result;
    }
};

// ============================================================
// Profile 07: CRC-64, 32-bit counter, 32-bit data ID, 32-bit length
//
// Header layout (20 bytes prepended to data):
//   Bytes  0-3:   Length (32 bit, big-endian)
//   Bytes  4-7:   Counter (32 bit, big-endian)
//   Bytes  8-11:  Data ID (32 bit, big-endian)
//   Bytes 12-19:  CRC-64 (64 bit, big-endian)
// ============================================================

struct profile_07_config {
    std::uint32_t data_id{0};
    std::uint32_t min_data_length{20};
    std::uint32_t max_data_length{65536};
    std::uint32_t max_delta_counter{1};
};

struct profile_07 {
    using config_type = profile_07_config;
    static constexpr std::size_t header_size = 20;

    static void write_u32_be(std::byte* p, std::uint32_t v) noexcept {
        p[0] = static_cast<std::byte>(v >> 24);
        p[1] = static_cast<std::byte>(v >> 16);
        p[2] = static_cast<std::byte>(v >> 8);
        p[3] = static_cast<std::byte>(v);
    }
    static void write_u64_be(std::byte* p, std::uint64_t v) noexcept {
        for (int i = 7; i >= 0; --i)
            p[7 - i] = static_cast<std::byte>(v >> (i * 8));
    }
    static std::uint32_t read_u32_be(const std::byte* p) noexcept {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8)  |
                static_cast<std::uint32_t>(p[3]);
    }
    static std::uint64_t read_u64_be(const std::byte* p) noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | static_cast<std::uint64_t>(p[i]);
        return v;
    }

    static void protect(std::span<std::byte> payload,
                        const profile_07_config& cfg,
                        std::uint32_t counter) noexcept {
        if (payload.size() < header_size) return;

        write_u32_be(payload.data() + 0, static_cast<std::uint32_t>(payload.size()));
        write_u32_be(payload.data() + 4, counter);
        write_u32_be(payload.data() + 8, cfg.data_id);

        // Zero CRC field
        std::memset(payload.data() + 12, 0, 8);

        std::uint64_t c = crc64(payload);
        write_u64_be(payload.data() + 12, c);
    }

    static e2e_result check(std::span<const std::byte> payload,
                            const profile_07_config& cfg,
                            std::uint32_t& last_counter) noexcept {
        if (payload.size() < header_size) return e2e_result::error;

        std::uint32_t recv_len = read_u32_be(payload.data());
        if (recv_len != payload.size()) return e2e_result::error;

        std::uint32_t recv_counter = read_u32_be(payload.data() + 4);
        std::uint32_t recv_data_id = read_u32_be(payload.data() + 8);
        if (recv_data_id != cfg.data_id) return e2e_result::wrong_crc;

        std::uint64_t recv_crc = read_u64_be(payload.data() + 12);

        // Recompute with CRC field zeroed using fast slicing-by-8.
        // Copy payload, zero the CRC field, compute.
        // For typical E2E payloads (<4KB), stack copy is fine.
        auto buf = std::vector<std::byte>(payload.begin(), payload.end());
        std::memset(buf.data() + 12, 0, 8);
        std::uint64_t c = crc64(std::span<const std::byte>{buf});

        if (c != recv_crc) return e2e_result::wrong_crc;

        std::uint32_t delta = recv_counter - last_counter;
        e2e_result result;
        if (delta == 0)
            result = e2e_result::repeated;
        else if (delta <= cfg.max_delta_counter)
            result = e2e_result::ok;
        else
            result = e2e_result::wrong_sequence;
        last_counter = recv_counter;
        return result;
    }
};

} // namespace bsomeip::e2e
