// SPDX-License-Identifier: MIT
// SOME/IP-SD message: serialize and deserialize the SD payload
// (flags + entries array + options array).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <expected>

#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/sd/entry.hpp>
#include <bsomeip/sd/option.hpp>

namespace bsomeip::sd {

// SD flags byte
struct sd_flags {
    bool reboot  : 1 = false;  // bit 7
    bool unicast : 1 = true;   // bit 6

    constexpr std::uint8_t to_byte() const noexcept {
        return static_cast<std::uint8_t>(
            (reboot  ? 0x80 : 0) |
            (unicast ? 0x40 : 0));
    }

    static constexpr sd_flags from_byte(std::uint8_t b) noexcept {
        return {
            .reboot  = (b & 0x80) != 0,
            .unicast = (b & 0x40) != 0,
        };
    }
};

// SD payload minimum size: flags(1) + reserved(3) + entries_length(4) + options_length(4) = 12
inline constexpr std::size_t sd_min_payload_size = 12;

// Parsed SD message (payload only — the SOME/IP header is handled separately)
struct sd_message {
    sd_flags flags{};
    std::vector<sd_entry> entries;
    std::vector<sd_option> options;
};

enum class sd_error : std::uint8_t {
    ok = 0,
    buffer_too_small,
    invalid_entries_length,
    invalid_options_length,
    truncated_entry,
    truncated_option,
    unknown_option_type,
};

// Deserialize SD payload (everything after the 16-byte SOME/IP header).
inline std::expected<sd_message, sd_error>
deserialize_sd(std::span<const std::byte> payload) noexcept {
    if (payload.size() < sd_min_payload_size)
        return std::unexpected(sd_error::buffer_too_small);

    const auto* p = payload.data();

    sd_message msg;
    msg.flags = sd_flags::from_byte(static_cast<std::uint8_t>(p[0]));
    // p[1..3] = reserved

    auto entries_len = wire::read_u32_be(p + 4);
    if (8 + entries_len + 4 > payload.size())
        return std::unexpected(sd_error::invalid_entries_length);
    if (entries_len % entry_size != 0)
        return std::unexpected(sd_error::invalid_entries_length);

    // Parse entries
    std::size_t num_entries = entries_len / entry_size;
    msg.entries.reserve(num_entries);
    std::size_t off = 8;
    for (std::size_t i = 0; i < num_entries; ++i) {
        if (off + entry_size > payload.size())
            return std::unexpected(sd_error::truncated_entry);
        msg.entries.push_back(read_entry(payload.subspan(off, entry_size)));
        off += entry_size;
    }

    // Options length
    if (off + 4 > payload.size())
        return std::unexpected(sd_error::invalid_options_length);
    auto options_len = wire::read_u32_be(p + off);
    off += 4;

    if (off + options_len > payload.size())
        return std::unexpected(sd_error::invalid_options_length);

    // Parse options
    std::size_t options_end = off + options_len;
    while (off < options_end) {
        if (off + option_header_size > options_end)
            return std::unexpected(sd_error::truncated_option);

        auto opt_size = read_option_size(payload.subspan(off));
        if (off + opt_size > options_end)
            return std::unexpected(sd_error::truncated_option);

        auto otype = peek_option_type(payload.subspan(off));
        switch (otype) {
        case option_type::ipv4_endpoint:
        case option_type::ipv4_multicast:
            if (opt_size < ipv4_option_size)
                return std::unexpected(sd_error::truncated_option);
            msg.options.push_back(
                read_ipv4_option(payload.subspan(off, ipv4_option_size)));
            break;
        case option_type::ipv6_endpoint:
        case option_type::ipv6_multicast:
            if (opt_size < ipv6_option_size)
                return std::unexpected(sd_error::truncated_option);
            msg.options.push_back(
                read_ipv6_option(payload.subspan(off, ipv6_option_size)));
            break;
        default:
            // Skip unknown option types (forward-compat)
            break;
        }
        off += opt_size;
    }

    return msg;
}

// Serialize SD payload into a byte buffer.
// Returns the number of bytes written, or 0 if the buffer is too small.
inline std::size_t serialize_sd(std::span<std::byte> buf,
                                const sd_message& msg) noexcept {
    // Calculate total size
    std::size_t entries_len = msg.entries.size() * entry_size;
    std::size_t options_len = 0;
    for (const auto& opt : msg.options) {
        std::visit([&](const auto& o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, ipv4_option>)
                options_len += ipv4_option_size;
            else
                options_len += ipv6_option_size;
        }, opt);
    }

    std::size_t total = sd_min_payload_size + entries_len + options_len;
    if (buf.size() < total) return 0;

    auto* p = buf.data();

    // Flags + reserved
    p[0] = static_cast<std::byte>(msg.flags.to_byte());
    p[1] = std::byte{0};
    p[2] = std::byte{0};
    p[3] = std::byte{0};

    // Entries length
    wire::write_u32_be(p + 4, static_cast<std::uint32_t>(entries_len));

    // Entries
    std::size_t off = 8;
    for (const auto& e : msg.entries) {
        write_entry(buf.subspan(off, entry_size), e);
        off += entry_size;
    }

    // Options length
    wire::write_u32_be(p + off, static_cast<std::uint32_t>(options_len));
    off += 4;

    // Options
    for (const auto& opt : msg.options) {
        off += write_option(buf.subspan(off), opt);
    }

    return total;
}

// Build a complete SD SOME/IP message (header + payload) in a buffer.
// Returns the total message size, or 0 if the buffer is too small.
inline std::size_t build_sd_message(
    std::span<std::byte> buf, const sd_message& msg,
    wire::session_id session = wire::session_id{1}) noexcept
{
    // Serialize payload first (after header)
    if (buf.size() < wire::header_size)
        return 0;

    auto payload_buf = buf.subspan(wire::header_size);
    auto payload_len = serialize_sd(payload_buf, msg);
    if (payload_len == 0 && !msg.entries.empty())
        return 0;
    // Empty SD message is valid (just flags + empty arrays)
    if (payload_len == 0)
        payload_len = serialize_sd(payload_buf, msg);

    // Fill the SOME/IP header
    wire::header_view hdr{buf.data()};
    hdr.set_service(wire::sd_service);
    hdr.set_method(wire::sd_method);
    hdr.set_payload_length(static_cast<wire::length_t>(payload_len));
    hdr.set_client(wire::sd_client);
    hdr.set_session(session);
    hdr.set_protocol_ver(wire::protocol_version);
    hdr.set_interface_ver(0x01);
    hdr.set_msg_type(wire::message_type::notification);
    hdr.set_ret_code(wire::return_code::e_ok);

    return wire::header_size + payload_len;
}

} // namespace bsomeip::sd
