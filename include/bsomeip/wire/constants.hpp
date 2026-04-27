// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include <bsomeip/wire/types.hpp>

namespace bsomeip::wire {

// Protocol constants
inline constexpr protocol_version_t protocol_version = 0x01;

// Header field sizes and offsets
inline constexpr std::size_t header_size           = 16;
inline constexpr std::size_t someip_header_size    = 8;  // Service(2) + Method(2) + Length(4)

inline constexpr std::size_t service_id_offset     = 0;
inline constexpr std::size_t method_id_offset      = 2;
inline constexpr std::size_t length_offset         = 4;
inline constexpr std::size_t client_id_offset      = 8;
inline constexpr std::size_t session_id_offset     = 10;
inline constexpr std::size_t protocol_ver_offset   = 12;
inline constexpr std::size_t interface_ver_offset   = 13;
inline constexpr std::size_t message_type_offset   = 14;
inline constexpr std::size_t return_code_offset    = 15;
inline constexpr std::size_t payload_offset        = 16;

// Maximum sizes
inline constexpr std::size_t max_udp_message_size  = 1416;
inline constexpr std::size_t max_tcp_message_size  = 4096 * 1024; // 4 MiB

// TP (Transport Protocol)
inline constexpr std::size_t tp_header_size        = 4;
inline constexpr std::size_t tp_payload_offset     = payload_offset + tp_header_size;
inline constexpr std::size_t tp_max_segment_length = 1392;
inline constexpr std::uint8_t tp_flag              = 0x20;

// Magic cookies
inline constexpr std::uint16_t magic_cookie_client_service = 0xFFFF;
inline constexpr std::uint16_t magic_cookie_client_method  = 0x0000;
inline constexpr std::uint16_t magic_cookie_server_method  = 0x8000;
inline constexpr std::uint32_t magic_cookie_value          = 0xDEADBEEF;

// Service Discovery
inline constexpr service_id sd_service{0xFFFF};
inline constexpr method_id  sd_method{0x8100};
inline constexpr client_id  sd_client{0x0000};
inline constexpr instance_id sd_instance{0x0000};

// Wildcard IDs
inline constexpr service_id    any_service{0xFFFF};
inline constexpr instance_id   any_instance{0xFFFF};
inline constexpr method_id     any_method{0xFFFF};
inline constexpr event_id      any_event{0xFFFF};
inline constexpr client_id     any_client{0xFFFF};
inline constexpr eventgroup_id any_eventgroup{0xFFFF};
inline constexpr interface_version_t any_major = 0xFF;
inline constexpr std::uint32_t any_minor = 0xFFFFFFFF;

} // namespace bsomeip::wire
