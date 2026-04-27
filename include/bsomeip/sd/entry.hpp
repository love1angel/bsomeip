// SPDX-License-Identifier: MIT
// SOME/IP-SD entry types and wire codec.
// Each entry is exactly 16 bytes on wire.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <bsomeip/wire/header.hpp>

namespace bsomeip::sd {

// SD entry type identifiers (byte 0 of each entry)
enum class entry_type : std::uint8_t {
    find_service               = 0x00,
    offer_service              = 0x01,  // TTL=0 → stop_offer
    subscribe_eventgroup       = 0x06,  // TTL=0 → stop_subscribe
    subscribe_eventgroup_ack   = 0x07,  // TTL=0 → nack
};

inline constexpr std::size_t entry_size = 16;

constexpr bool is_service_entry(entry_type t) noexcept {
    auto v = static_cast<std::uint8_t>(t);
    return v <= 0x02;
}

constexpr bool is_eventgroup_entry(entry_type t) noexcept {
    auto v = static_cast<std::uint8_t>(t);
    return v >= 0x04;
}

// Parsed SD entry — covers both service and eventgroup variants.
struct sd_entry {
    entry_type type{};

    // Option run indices and counts
    std::uint8_t index_1st{};
    std::uint8_t index_2nd{};
    std::uint8_t num_options_1st{};  // high nibble of byte 3
    std::uint8_t num_options_2nd{};  // low nibble of byte 3

    wire::service_id  service{};
    wire::instance_id instance{};
    std::uint8_t      major_version{};
    std::uint32_t     ttl{};  // 24-bit on wire

    // Service entries (find/offer): minor_version occupies bytes 12-15
    std::uint32_t minor_version{};

    // Eventgroup entries (subscribe/ack): eventgroup_id in bytes 14-15
    wire::eventgroup_id eventgroup{};
};

// Read a 16-byte SD entry from a buffer.
// Caller must ensure buf.size() >= 16.
constexpr sd_entry read_entry(std::span<const std::byte> buf) noexcept {
    using wire::read_u16_be;
    using wire::read_u32_be;
    const auto* p = buf.data();

    sd_entry e;
    e.type = static_cast<entry_type>(p[0]);
    e.index_1st = static_cast<std::uint8_t>(p[1]);
    e.index_2nd = static_cast<std::uint8_t>(p[2]);
    auto opts_byte = static_cast<std::uint8_t>(p[3]);
    e.num_options_1st = (opts_byte >> 4) & 0x0F;
    e.num_options_2nd = opts_byte & 0x0F;

    e.service  = wire::service_id{read_u16_be(p + 4)};
    e.instance = wire::instance_id{read_u16_be(p + 6)};

    // Byte 8: major_version
    // Bytes 9-11: TTL (24-bit big-endian)
    e.major_version = static_cast<std::uint8_t>(p[8]);
    e.ttl = (static_cast<std::uint32_t>(p[9])  << 16) |
            (static_cast<std::uint32_t>(p[10]) << 8)  |
             static_cast<std::uint32_t>(p[11]);

    if (is_service_entry(e.type)) {
        e.minor_version = read_u32_be(p + 12);
    } else {
        // bytes 12-13: reserved (includes counter nibble, ignored here)
        e.eventgroup = wire::eventgroup_id{read_u16_be(p + 14)};
    }
    return e;
}

// Write a 16-byte SD entry to a buffer.
// Caller must ensure buf.size() >= 16.
constexpr void write_entry(std::span<std::byte> buf,
                           const sd_entry& e) noexcept {
    using wire::write_u16_be;
    using wire::write_u32_be;
    auto* p = buf.data();

    p[0] = static_cast<std::byte>(e.type);
    p[1] = static_cast<std::byte>(e.index_1st);
    p[2] = static_cast<std::byte>(e.index_2nd);
    p[3] = static_cast<std::byte>((e.num_options_1st << 4) | (e.num_options_2nd & 0x0F));

    write_u16_be(p + 4, e.service.value);
    write_u16_be(p + 6, e.instance.value);

    p[8] = static_cast<std::byte>(e.major_version);
    p[9]  = static_cast<std::byte>((e.ttl >> 16) & 0xFF);
    p[10] = static_cast<std::byte>((e.ttl >> 8) & 0xFF);
    p[11] = static_cast<std::byte>(e.ttl & 0xFF);

    if (is_service_entry(e.type)) {
        write_u32_be(p + 12, e.minor_version);
    } else {
        // reserved bytes 12-13
        p[12] = std::byte{0};
        p[13] = std::byte{0};
        write_u16_be(p + 14, e.eventgroup.value);
    }
}

// --- Convenience constructors ---

constexpr sd_entry make_find_service(
    wire::service_id service, wire::instance_id instance,
    std::uint8_t major, std::uint32_t minor,
    std::uint32_t ttl = 3) noexcept
{
    return {
        .type = entry_type::find_service,
        .service = service,
        .instance = instance,
        .major_version = major,
        .ttl = ttl,
        .minor_version = minor,
    };
}

constexpr sd_entry make_offer_service(
    wire::service_id service, wire::instance_id instance,
    std::uint8_t major, std::uint32_t minor,
    std::uint32_t ttl = 3) noexcept
{
    return {
        .type = entry_type::offer_service,
        .service = service,
        .instance = instance,
        .major_version = major,
        .ttl = ttl,
        .minor_version = minor,
    };
}

constexpr sd_entry make_stop_offer(
    wire::service_id service, wire::instance_id instance,
    std::uint8_t major, std::uint32_t minor) noexcept
{
    return make_offer_service(service, instance, major, minor, /*ttl=*/0);
}

constexpr sd_entry make_subscribe(
    wire::service_id service, wire::instance_id instance,
    std::uint8_t major, wire::eventgroup_id eg,
    std::uint32_t ttl = 3) noexcept
{
    return {
        .type = entry_type::subscribe_eventgroup,
        .service = service,
        .instance = instance,
        .major_version = major,
        .ttl = ttl,
        .eventgroup = eg,
    };
}

constexpr sd_entry make_subscribe_ack(
    wire::service_id service, wire::instance_id instance,
    std::uint8_t major, wire::eventgroup_id eg,
    std::uint32_t ttl = 3) noexcept
{
    return {
        .type = entry_type::subscribe_eventgroup_ack,
        .service = service,
        .instance = instance,
        .major_version = major,
        .ttl = ttl,
        .eventgroup = eg,
    };
}

constexpr sd_entry make_subscribe_nack(
    wire::service_id service, wire::instance_id instance,
    std::uint8_t major, wire::eventgroup_id eg) noexcept
{
    return make_subscribe_ack(service, instance, major, eg, /*ttl=*/0);
}

} // namespace bsomeip::sd
