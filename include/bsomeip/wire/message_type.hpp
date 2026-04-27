// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace bsomeip::wire {

// SOME/IP message types (byte 14 of header)
enum class message_type : std::uint8_t {
    request              = 0x00,
    request_no_return    = 0x01,
    notification         = 0x02,
    request_ack          = 0x40,
    request_no_return_ack = 0x41,
    notification_ack     = 0x42,
    response             = 0x80,
    error                = 0x81,
    response_ack         = 0xC0,
    error_ack            = 0xC1,
};

// Check if a message type is a request (expects response)
constexpr bool is_request(message_type t) noexcept {
    return t == message_type::request || t == message_type::request_ack;
}

// Check if a message type is a response
constexpr bool is_response(message_type t) noexcept {
    auto v = static_cast<std::uint8_t>(t);
    return (v & 0x80) != 0;
}

// Check if a message type has TP (Transport Protocol) flag
constexpr bool is_tp(message_type t) noexcept {
    auto v = static_cast<std::uint8_t>(t);
    return (v & 0x20) != 0;
}

// Set TP flag on a message type
constexpr message_type with_tp(message_type t) noexcept {
    return static_cast<message_type>(static_cast<std::uint8_t>(t) | 0x20);
}

// Clear TP flag
constexpr message_type without_tp(message_type t) noexcept {
    return static_cast<message_type>(static_cast<std::uint8_t>(t) & ~0x20);
}

} // namespace bsomeip::wire
