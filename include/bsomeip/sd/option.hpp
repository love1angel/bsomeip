// SPDX-License-Identifier: MIT
// SOME/IP-SD option types and wire codec.
// Options follow the entries array in an SD message.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <array>

#include <bsomeip/wire/header.hpp>

namespace bsomeip::sd {

// Option type identifiers
enum class option_type : std::uint8_t {
    configuration   = 0x01,
    load_balancing  = 0x02,
    protection      = 0x03,
    ipv4_endpoint   = 0x04,
    ipv6_endpoint   = 0x06,
    ipv4_multicast  = 0x14,
    ipv6_multicast  = 0x16,
    selective       = 0x20,
};

// Transport protocol identifiers used in endpoint options
enum class l4_protocol : std::uint8_t {
    tcp = 0x06,
    udp = 0x11,
};

// Option header: 2-byte length + 1-byte type + 1-byte reserved = 4 bytes
inline constexpr std::size_t option_header_size = 4;

// IPv4 endpoint/multicast option: header(4) + address(4) + reserved(1) + proto(1) + port(2) = 12
inline constexpr std::size_t ipv4_option_size = 12;

// IPv6 endpoint/multicast option: header(4) + address(16) + reserved(1) + proto(1) + port(2) = 24
inline constexpr std::size_t ipv6_option_size = 24;

// Parsed IPv4 endpoint option
struct ipv4_option {
    option_type type{option_type::ipv4_endpoint};
    std::array<std::uint8_t, 4> address{};
    l4_protocol protocol{l4_protocol::udp};
    std::uint16_t port{};
};

// Parsed IPv6 endpoint option
struct ipv6_option {
    option_type type{option_type::ipv6_endpoint};
    std::array<std::uint8_t, 16> address{};
    l4_protocol protocol{l4_protocol::udp};
    std::uint16_t port{};
};

// Generic option — covers all known types
using sd_option = std::variant<ipv4_option, ipv6_option>;

// Read the option type from a buffer (peek at byte 2)
constexpr option_type peek_option_type(std::span<const std::byte> buf) noexcept {
    return static_cast<option_type>(buf[2]);
}

// Read the total size of an option (length field + 3)
constexpr std::size_t read_option_size(std::span<const std::byte> buf) noexcept {
    auto len = wire::read_u16_be(buf.data());
    return static_cast<std::size_t>(len) + 3;
}

// Read an IPv4 endpoint/multicast option from a 12-byte buffer.
constexpr ipv4_option read_ipv4_option(std::span<const std::byte> buf) noexcept {
    const auto* p = buf.data();
    ipv4_option opt;
    opt.type = static_cast<option_type>(p[2]);
    // p[3] = reserved
    opt.address[0] = static_cast<std::uint8_t>(p[4]);
    opt.address[1] = static_cast<std::uint8_t>(p[5]);
    opt.address[2] = static_cast<std::uint8_t>(p[6]);
    opt.address[3] = static_cast<std::uint8_t>(p[7]);
    // p[8] = reserved
    opt.protocol = static_cast<l4_protocol>(p[9]);
    opt.port = wire::read_u16_be(p + 10);
    return opt;
}

// Read an IPv6 endpoint/multicast option from a 24-byte buffer.
constexpr ipv6_option read_ipv6_option(std::span<const std::byte> buf) noexcept {
    const auto* p = buf.data();
    ipv6_option opt;
    opt.type = static_cast<option_type>(p[2]);
    for (int i = 0; i < 16; ++i)
        opt.address[i] = static_cast<std::uint8_t>(p[4 + i]);
    // p[20] = reserved
    opt.protocol = static_cast<l4_protocol>(p[21]);
    opt.port = wire::read_u16_be(p + 22);
    return opt;
}

// Write an IPv4 endpoint/multicast option to a 12-byte buffer.
constexpr void write_ipv4_option(std::span<std::byte> buf,
                                 const ipv4_option& opt) noexcept {
    auto* p = buf.data();
    // length = 9 (total 12 - 3 header bytes)
    wire::write_u16_be(p, 0x0009);
    p[2] = static_cast<std::byte>(opt.type);
    p[3] = std::byte{0};  // reserved
    p[4] = static_cast<std::byte>(opt.address[0]);
    p[5] = static_cast<std::byte>(opt.address[1]);
    p[6] = static_cast<std::byte>(opt.address[2]);
    p[7] = static_cast<std::byte>(opt.address[3]);
    p[8] = std::byte{0};  // reserved
    p[9] = static_cast<std::byte>(opt.protocol);
    wire::write_u16_be(p + 10, opt.port);
}

// Write an IPv6 endpoint/multicast option to a 24-byte buffer.
constexpr void write_ipv6_option(std::span<std::byte> buf,
                                 const ipv6_option& opt) noexcept {
    auto* p = buf.data();
    // length = 21 (total 24 - 3 header bytes)
    wire::write_u16_be(p, 0x0015);
    p[2] = static_cast<std::byte>(opt.type);
    p[3] = std::byte{0};  // reserved
    for (int i = 0; i < 16; ++i)
        p[4 + i] = static_cast<std::byte>(opt.address[i]);
    p[20] = std::byte{0};  // reserved
    p[21] = static_cast<std::byte>(opt.protocol);
    wire::write_u16_be(p + 22, opt.port);
}

// Write any sd_option variant
constexpr std::size_t write_option(std::span<std::byte> buf,
                                   const sd_option& opt) noexcept {
    return std::visit([&](const auto& o) -> std::size_t {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, ipv4_option>) {
            write_ipv4_option(buf, o);
            return ipv4_option_size;
        } else {
            write_ipv6_option(buf, o);
            return ipv6_option_size;
        }
    }, opt);
}

// --- Convenience constructors ---

constexpr ipv4_option make_ipv4_endpoint(
    std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
    l4_protocol proto, std::uint16_t port) noexcept
{
    return {
        .type = option_type::ipv4_endpoint,
        .address = {a, b, c, d},
        .protocol = proto,
        .port = port,
    };
}

constexpr ipv4_option make_ipv4_multicast(
    std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
    l4_protocol proto, std::uint16_t port) noexcept
{
    return {
        .type = option_type::ipv4_multicast,
        .address = {a, b, c, d},
        .protocol = proto,
        .port = port,
    };
}

} // namespace bsomeip::sd
