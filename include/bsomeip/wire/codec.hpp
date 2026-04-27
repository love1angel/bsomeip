// SPDX-License-Identifier: MIT
// Reflection-based zero-copy SOME/IP codec.
// Generates serialize/deserialize at compile time for any aggregate struct
// using C++26 std::meta reflection.
#pragma once

#include <meta>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <expected>
#include <utility>

#include <bsomeip/wire/header.hpp>

namespace bsomeip::wire {

enum class codec_error : std::uint8_t {
    ok = 0,
    buffer_too_small,
    invalid_length,
    unsupported_type,
};

// --- Reflection helpers (index-based to avoid vector-in-constexpr issue) ---
//
// TODO(gcc16-workaround): GCC 16.0.1 does not support `template for` (expansion
// statements) or range-for over nonstatic_data_members_of() in non-consteval
// contexts — the returned std::vector<info> triggers a "not a constant expression
// because it refers to a result of operator new" error.  As a workaround we use
// consteval index helpers + index_sequence fold-expression to iterate members.
// Replace with `template for (auto m : nsdm_of(^^T)) { ... }` once the compiler
// supports P1306 expansion statements properly.
//
// TODO(gcc16-api): nonstatic_data_members_of() requires a second
// `access_context` argument in GCC 16's <meta>.  The P2996 paper lists it as a
// single-argument function.  Switch to the single-argument form when the API
// stabilises.

template <typename T>
consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(^^T,
        std::meta::access_context::unchecked()).size();
}

template <typename T>
consteval std::meta::info get_member(std::size_t i) {
    return std::meta::nonstatic_data_members_of(^^T,
        std::meta::access_context::unchecked())[i];
}

// --- Serialization: struct → big-endian bytes ---

constexpr std::size_t serialize_primitive(std::byte* out, std::uint8_t v) noexcept {
    out[0] = static_cast<std::byte>(v);
    return 1;
}

constexpr std::size_t serialize_primitive(std::byte* out, bool v) noexcept {
    out[0] = static_cast<std::byte>(v ? 1 : 0);
    return 1;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::int8_t v) noexcept {
    out[0] = static_cast<std::byte>(v);
    return 1;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::uint16_t v) noexcept {
    write_u16_be(out, v);
    return 2;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::int16_t v) noexcept {
    write_u16_be(out, static_cast<std::uint16_t>(v));
    return 2;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::uint32_t v) noexcept {
    write_u32_be(out, v);
    return 4;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::int32_t v) noexcept {
    write_u32_be(out, static_cast<std::uint32_t>(v));
    return 4;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::uint64_t v) noexcept {
    write_u32_be(out, static_cast<std::uint32_t>(v >> 32));
    write_u32_be(out + 4, static_cast<std::uint32_t>(v));
    return 8;
}

constexpr std::size_t serialize_primitive(std::byte* out, std::int64_t v) noexcept {
    return serialize_primitive(out, static_cast<std::uint64_t>(v));
}

constexpr std::size_t serialize_primitive(std::byte* out, float v) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    write_u32_be(out, bits);
    return 4;
}

constexpr std::size_t serialize_primitive(std::byte* out, double v) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    return serialize_primitive(out, bits);
}

// --- Deserialization: big-endian bytes → value ---

constexpr std::uint8_t deserialize_u8(const std::byte* p) noexcept {
    return static_cast<std::uint8_t>(p[0]);
}

constexpr std::int8_t deserialize_i8(const std::byte* p) noexcept {
    return static_cast<std::int8_t>(p[0]);
}

constexpr bool deserialize_bool(const std::byte* p) noexcept {
    return static_cast<std::uint8_t>(p[0]) != 0;
}

constexpr std::uint16_t deserialize_u16(const std::byte* p) noexcept {
    return read_u16_be(p);
}

constexpr std::int16_t deserialize_i16(const std::byte* p) noexcept {
    return static_cast<std::int16_t>(read_u16_be(p));
}

constexpr std::uint32_t deserialize_u32(const std::byte* p) noexcept {
    return read_u32_be(p);
}

constexpr std::int32_t deserialize_i32(const std::byte* p) noexcept {
    return static_cast<std::int32_t>(read_u32_be(p));
}

constexpr std::uint64_t deserialize_u64(const std::byte* p) noexcept {
    return (static_cast<std::uint64_t>(read_u32_be(p)) << 32) |
            static_cast<std::uint64_t>(read_u32_be(p + 4));
}

constexpr std::int64_t deserialize_i64(const std::byte* p) noexcept {
    return static_cast<std::int64_t>(deserialize_u64(p));
}

constexpr float deserialize_f32(const std::byte* p) noexcept {
    std::uint32_t bits = read_u32_be(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

constexpr double deserialize_f64(const std::byte* p) noexcept {
    std::uint64_t bits = deserialize_u64(p);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

// --- Reflection-driven serialize/deserialize for aggregate types ---

template <typename T>
constexpr std::size_t serialize_field(std::byte* out, const T& value) noexcept;

template <typename T>
constexpr std::size_t deserialize_field(const std::byte* in, T& value) noexcept;

// TODO(gcc16-workaround): These use index_sequence fold-expression instead of
// the natural `template for` expansion.  See comment above on get_member().
template <typename T, std::size_t... Is>
constexpr std::size_t serialize_members(std::byte* out, const T& value,
                                        std::index_sequence<Is...>) noexcept {
    std::size_t offset = 0;
    ((offset += serialize_field(out + offset, value.[:get_member<T>(Is):])), ...);
    return offset;
}

template <typename T, std::size_t... Is>
constexpr std::size_t deserialize_members(const std::byte* in, T& value,
                                          std::index_sequence<Is...>) noexcept {
    std::size_t offset = 0;
    ((offset += deserialize_field(in + offset, value.[:get_member<T>(Is):])), ...);
    return offset;
}

template <typename T>
constexpr std::size_t serialize_field(std::byte* out, const T& value) noexcept {
    if constexpr (std::is_enum_v<T>) {
        return serialize_field(out, static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::uint8_t> ||
                         std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint16_t> ||
                         std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::uint32_t> ||
                         std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::uint64_t> ||
                         std::is_same_v<T, std::int64_t> || std::is_same_v<T, float> ||
                         std::is_same_v<T, double>) {
        return serialize_primitive(out, value);
    } else if constexpr (requires { typename T::value_type; { value.size() }; { value.data() }; }
                         && std::is_trivial_v<typename T::value_type>
                         && sizeof(typename T::value_type) == 1) {
        // Fixed-size byte arrays (std::array<uint8_t, N>, etc.): raw copy.
        std::memcpy(out, value.data(), value.size());
        return value.size();
    } else if constexpr (std::is_aggregate_v<T>) {
        return serialize_members(out, value,
            std::make_index_sequence<member_count<T>()>{});
    } else {
        static_assert(false, "unsupported type for serialize_field");
    }
}

template <typename T>
constexpr std::size_t deserialize_field(const std::byte* in, T& value) noexcept {
    if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> tmp;
        auto n = deserialize_field(in, tmp);
        value = static_cast<T>(tmp);
        return n;
    } else if constexpr (std::is_same_v<T, bool>) {
        value = deserialize_bool(in); return 1;
    } else if constexpr (std::is_same_v<T, std::uint8_t>) {
        value = deserialize_u8(in); return 1;
    } else if constexpr (std::is_same_v<T, std::int8_t>) {
        value = deserialize_i8(in); return 1;
    } else if constexpr (std::is_same_v<T, std::uint16_t>) {
        value = deserialize_u16(in); return 2;
    } else if constexpr (std::is_same_v<T, std::int16_t>) {
        value = deserialize_i16(in); return 2;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        value = deserialize_u32(in); return 4;
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        value = deserialize_i32(in); return 4;
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        value = deserialize_u64(in); return 8;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        value = deserialize_i64(in); return 8;
    } else if constexpr (std::is_same_v<T, float>) {
        value = deserialize_f32(in); return 4;
    } else if constexpr (std::is_same_v<T, double>) {
        value = deserialize_f64(in); return 8;
    } else if constexpr (requires { typename T::value_type; { value.size() }; { value.data() }; }
                         && std::is_trivial_v<typename T::value_type>
                         && sizeof(typename T::value_type) == 1) {
        // Fixed-size byte arrays (std::array<uint8_t, N>, etc.): raw copy.
        std::memcpy(value.data(), in, value.size());
        return value.size();
    } else if constexpr (std::is_aggregate_v<T>) {
        return deserialize_members(in, value,
            std::make_index_sequence<member_count<T>()>{});
    } else {
        static_assert(false, "unsupported type for deserialize_field");
    }
}

// --- Public API ---

// TODO(gcc16-workaround): serialize_checked / deserialize_checked use the same
// index_sequence workaround as serialize_members / deserialize_members.
template <typename T, std::size_t... Is>
constexpr std::expected<std::size_t, codec_error>
serialize_checked(std::span<std::byte> buf, const T& value,
                  std::index_sequence<Is...>) noexcept {
    std::size_t offset = 0;
    codec_error err = codec_error::ok;
    auto step = [&]<std::size_t I>() {
        if (err != codec_error::ok) return;
        auto n = serialize_field(buf.data() + offset, value.[:get_member<T>(I):]);
        offset += n;
        if (offset > buf.size()) err = codec_error::buffer_too_small;
    };
    (step.template operator()<Is>(), ...);
    if (err != codec_error::ok) return std::unexpected{err};
    return offset;
}

template <typename T>
    requires std::is_aggregate_v<T>
constexpr std::expected<std::size_t, codec_error>
serialize(std::span<std::byte> buf, const T& value) noexcept {
    return serialize_checked(buf, value,
        std::make_index_sequence<member_count<T>()>{});
}

template <typename T, std::size_t... Is>
constexpr std::expected<T, codec_error>
deserialize_checked(std::span<const std::byte> buf,
                    std::index_sequence<Is...>) noexcept {
    T value{};
    std::size_t offset = 0;
    codec_error err = codec_error::ok;
    auto step = [&]<std::size_t I>() {
        if (err != codec_error::ok) return;
        if (offset >= buf.size()) { err = codec_error::buffer_too_small; return; }
        auto n = deserialize_field(buf.data() + offset, value.[:get_member<T>(I):]);
        offset += n;
    };
    (step.template operator()<Is>(), ...);
    if (err != codec_error::ok) return std::unexpected{err};
    return value;
}

template <typename T>
    requires std::is_aggregate_v<T>
constexpr std::expected<T, codec_error>
deserialize(std::span<const std::byte> buf) noexcept {
    return deserialize_checked<T>(buf,
        std::make_index_sequence<member_count<T>()>{});
}

template <typename T>
    requires std::is_aggregate_v<T>
constexpr std::expected<std::size_t, codec_error>
serialize_message(std::span<std::byte> buf, const header& hdr, const T& payload) noexcept {
    if (buf.size() < header_size) {
        return std::unexpected{codec_error::buffer_too_small};
    }
    for (std::size_t i = 0; i < header_size; ++i) {
        buf[i] = hdr.bytes[i];
    }
    auto result = serialize(buf.subspan(header_size), payload);
    if (!result) return result;
    header_view v{buf.data()};
    v.set_payload_length(static_cast<length_t>(*result));
    return header_size + *result;
}

template <typename T>
    requires std::is_aggregate_v<T>
constexpr std::expected<T, codec_error>
deserialize_payload(std::span<const std::byte> payload_buf) noexcept {
    return deserialize<T>(payload_buf);
}

} // namespace bsomeip::wire
