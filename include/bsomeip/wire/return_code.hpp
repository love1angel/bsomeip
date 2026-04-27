// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace bsomeip::wire {

// SOME/IP return codes (byte 15 of header)
enum class return_code : std::uint8_t {
    e_ok                     = 0x00,
    e_not_ok                 = 0x01,
    e_unknown_service        = 0x02,
    e_unknown_method         = 0x03,
    e_not_ready              = 0x04,
    e_not_reachable          = 0x05,
    e_timeout                = 0x06,
    e_wrong_protocol_version = 0x07,
    e_wrong_interface_version = 0x08,
    e_malformed_message      = 0x09,
    e_wrong_message_type     = 0x0A,
    // 0x0B-0x1F: reserved for SOME/IP
    // 0x20-0x5E: application-specific
};

constexpr bool is_error(return_code rc) noexcept {
    return rc != return_code::e_ok;
}

} // namespace bsomeip::wire
