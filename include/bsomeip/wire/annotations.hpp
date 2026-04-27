// SPDX-License-Identifier: MIT
// Annotations for reflection-based codec customization.
// Attach these to struct fields via C++26 [[=annotation]] syntax.
//
// TODO(gcc16-annotations): C++26 [[=expr]] annotation syntax and the
// corresponding std::meta::annotations_of() API are not yet exercised
// at compile time.  The codec currently ignores annotations and always
// serialises big-endian without length prefix.  Once GCC 16 stabilises
// annotation support, the codec should query annotations_of() per-member
// and dispatch to the appropriate encoding.
#pragma once

#include <cstdint>
#include <cstddef>

namespace bsomeip::wire {

// Field encoding annotations — applied via [[=annotation]] on struct members.

// Mark a field as big-endian (default for SOME/IP)
struct big_endian {};

// Mark a field as little-endian (rare, for host-order fields)
struct little_endian {};

// Prefix the field data with a length (in bytes of the specified type)
template <typename LenType = std::uint32_t>
struct length_prefix {};

// Fixed-size byte array (serialized as-is, no length prefix)
struct raw_bytes {};

// Skip this field during serialization (computed field)
struct skip {};

// Service definition annotation
struct service {
    std::uint16_t id;
    std::uint8_t major_version;
    std::uint32_t minor_version = 0;
};

// Method definition annotation
struct method {
    std::uint16_t id;
    bool reliable = true; // TCP (true) or UDP (false)
};

// Event definition annotation
struct event {
    std::uint16_t id;
    std::uint16_t eventgroup;
};

// Field definition annotation (for SOME/IP structured payload)
struct field {
    std::uint16_t id;
    bool has_getter = true;
    bool has_setter = false;
    bool has_notifier = false;
};

} // namespace bsomeip::wire
