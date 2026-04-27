// SPDX-License-Identifier: MIT
// bsomeip — C++26 SOME/IP implementation
#pragma once

#include <cstdint>
#include <compare>
#include <concepts>
#include <functional>

namespace bsomeip::wire {

// Strong type wrapper for SOME/IP identifiers.
// Provides type safety while remaining trivial/structural for use
// as template arguments and with std::meta reflection.
template <typename Tag, typename Underlying = std::uint16_t>
struct strong_id {
    Underlying value{};

    constexpr strong_id() = default;
    constexpr explicit strong_id(Underlying v) : value{v} {}

    constexpr auto operator<=>(const strong_id&) const = default;
    constexpr bool operator==(const strong_id&) const = default;

    constexpr explicit operator Underlying() const { return value; }
};

// SOME/IP identifier types
struct service_tag {};
struct method_tag {};
struct event_tag {};
struct instance_tag {};
struct eventgroup_tag {};
struct client_tag {};
struct session_tag {};

using service_id   = strong_id<service_tag>;
using method_id    = strong_id<method_tag>;
using event_id     = strong_id<event_tag>;
using instance_id  = strong_id<instance_tag>;
using eventgroup_id = strong_id<eventgroup_tag>;
using client_id    = strong_id<client_tag>;
using session_id   = strong_id<session_tag>;

// Scalar types used in headers
using length_t           = std::uint32_t;
using protocol_version_t = std::uint8_t;
using interface_version_t = std::uint8_t;
using ttl_t              = std::uint32_t;

} // namespace bsomeip::wire

// Hash support for use in unordered containers
template <typename Tag, typename U>
struct std::hash<bsomeip::wire::strong_id<Tag, U>> {
    constexpr std::size_t operator()(bsomeip::wire::strong_id<Tag, U> id) const noexcept {
        return std::hash<U>{}(id.value);
    }
};
