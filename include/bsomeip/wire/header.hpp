// SPDX-License-Identifier: MIT
// SOME/IP header: 16-byte on-wire format with zero-copy access.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/wire/constants.hpp>

namespace bsomeip::wire {

// Read a big-endian uint16 from buffer
constexpr std::uint16_t read_u16_be(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(p[0]) << 8) |
         static_cast<unsigned>(p[1]));
}

// Read a big-endian uint32 from buffer
constexpr std::uint32_t read_u32_be(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

// Write a big-endian uint16 to buffer
constexpr void write_u16_be(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>(v >> 8);
    p[1] = static_cast<std::byte>(v);
}

// Write a big-endian uint32 to buffer
constexpr void write_u32_be(std::byte* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::byte>(v >> 24);
    p[1] = static_cast<std::byte>(v >> 16);
    p[2] = static_cast<std::byte>(v >> 8);
    p[3] = static_cast<std::byte>(v);
}

// Zero-copy view of a SOME/IP header over a byte buffer.
// Does NOT own the memory — the caller must ensure the buffer outlives this view.
// All fields are read/written in network byte order (big-endian).
class header_view {
public:
    static constexpr std::size_t size = header_size;

    // Construct from a span (must be >= 16 bytes)
    constexpr explicit header_view(std::span<std::byte, header_size> buf) noexcept
        : data_{buf.data()} {}

    constexpr explicit header_view(std::byte* buf) noexcept
        : data_{buf} {}

    // --- Getters ---

    constexpr service_id service() const noexcept {
        return service_id{read_u16_be(data_ + service_id_offset)};
    }

    constexpr method_id method() const noexcept {
        return method_id{read_u16_be(data_ + method_id_offset)};
    }

    constexpr length_t length() const noexcept {
        return read_u32_be(data_ + length_offset);
    }

    constexpr client_id client() const noexcept {
        return client_id{read_u16_be(data_ + client_id_offset)};
    }

    constexpr session_id session() const noexcept {
        return session_id{read_u16_be(data_ + session_id_offset)};
    }

    constexpr protocol_version_t protocol_ver() const noexcept {
        return static_cast<protocol_version_t>(data_[protocol_ver_offset]);
    }

    constexpr interface_version_t interface_ver() const noexcept {
        return static_cast<interface_version_t>(data_[interface_ver_offset]);
    }

    constexpr message_type msg_type() const noexcept {
        return static_cast<message_type>(data_[message_type_offset]);
    }

    constexpr return_code ret_code() const noexcept {
        return static_cast<return_code>(data_[return_code_offset]);
    }

    // Payload length = length field - 8 (client + session + version fields)
    constexpr length_t payload_length() const noexcept {
        auto l = length();
        return l >= someip_header_size ? l - someip_header_size : 0;
    }

    // Total message size = header_size + payload_length
    constexpr std::size_t message_size() const noexcept {
        return header_size + payload_length();
    }

    // --- Setters ---

    constexpr void set_service(service_id v) noexcept {
        write_u16_be(data_ + service_id_offset, v.value);
    }

    constexpr void set_method(method_id v) noexcept {
        write_u16_be(data_ + method_id_offset, v.value);
    }

    constexpr void set_length(length_t v) noexcept {
        write_u32_be(data_ + length_offset, v);
    }

    constexpr void set_client(client_id v) noexcept {
        write_u16_be(data_ + client_id_offset, v.value);
    }

    constexpr void set_session(session_id v) noexcept {
        write_u16_be(data_ + session_id_offset, v.value);
    }

    constexpr void set_protocol_ver(protocol_version_t v) noexcept {
        data_[protocol_ver_offset] = static_cast<std::byte>(v);
    }

    constexpr void set_interface_ver(interface_version_t v) noexcept {
        data_[interface_ver_offset] = static_cast<std::byte>(v);
    }

    constexpr void set_msg_type(message_type v) noexcept {
        data_[message_type_offset] = static_cast<std::byte>(v);
    }

    constexpr void set_ret_code(return_code v) noexcept {
        data_[return_code_offset] = static_cast<std::byte>(v);
    }

    // Set length from payload size (adds 8 for protocol fields)
    constexpr void set_payload_length(length_t payload_len) noexcept {
        set_length(payload_len + someip_header_size);
    }

    // Access raw bytes
    constexpr std::byte* data() noexcept { return data_; }
    constexpr const std::byte* data() const noexcept { return data_; }

private:
    std::byte* data_;
};

// Read-only header view
class const_header_view {
public:
    constexpr explicit const_header_view(std::span<const std::byte, header_size> buf) noexcept
        : data_{buf.data()} {}

    constexpr explicit const_header_view(const std::byte* buf) noexcept
        : data_{buf} {}

    constexpr service_id service() const noexcept {
        return service_id{read_u16_be(data_ + service_id_offset)};
    }
    constexpr method_id method() const noexcept {
        return method_id{read_u16_be(data_ + method_id_offset)};
    }
    constexpr length_t length() const noexcept {
        return read_u32_be(data_ + length_offset);
    }
    constexpr client_id client() const noexcept {
        return client_id{read_u16_be(data_ + client_id_offset)};
    }
    constexpr session_id session() const noexcept {
        return session_id{read_u16_be(data_ + session_id_offset)};
    }
    constexpr protocol_version_t protocol_ver() const noexcept {
        return static_cast<protocol_version_t>(data_[protocol_ver_offset]);
    }
    constexpr interface_version_t interface_ver() const noexcept {
        return static_cast<interface_version_t>(data_[interface_ver_offset]);
    }
    constexpr message_type msg_type() const noexcept {
        return static_cast<message_type>(data_[message_type_offset]);
    }
    constexpr return_code ret_code() const noexcept {
        return static_cast<return_code>(data_[return_code_offset]);
    }
    constexpr length_t payload_length() const noexcept {
        auto l = length();
        return l >= someip_header_size ? l - someip_header_size : 0;
    }
    constexpr std::size_t message_size() const noexcept {
        return header_size + payload_length();
    }
    constexpr const std::byte* data() const noexcept { return data_; }

private:
    const std::byte* data_;
};

// Value type: self-contained header (owns 16 bytes)
struct header {
    std::byte bytes[header_size]{};

    constexpr header() = default;

    constexpr header_view view() noexcept { return header_view{bytes}; }
    constexpr const_header_view view() const noexcept { return const_header_view{bytes}; }

    // Convenience factory
    static constexpr header make(
        service_id service, method_id method,
        client_id client, session_id session,
        interface_version_t iface_ver = 0x01,
        message_type msg_type = message_type::request,
        return_code ret_code = return_code::e_ok) noexcept
    {
        header h;
        auto v = h.view();
        v.set_service(service);
        v.set_method(method);
        v.set_client(client);
        v.set_session(session);
        v.set_protocol_ver(protocol_version);
        v.set_interface_ver(iface_ver);
        v.set_msg_type(msg_type);
        v.set_ret_code(ret_code);
        v.set_length(someip_header_size); // no payload yet
        return h;
    }
};

} // namespace bsomeip::wire
