// SPDX-License-Identifier: MIT
// SOME/IP TCP stream framer.
// Reads complete SOME/IP messages from a TCP byte stream,
// handling message boundaries (length field) and magic cookies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <expected>
#include <functional>

#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>

namespace bsomeip::io {

// Result of a frame parse attempt.
enum class frame_result : std::uint8_t {
    ok,              // Complete message available
    need_more,       // Not enough data yet
    magic_cookie,    // Magic cookie detected and skipped
    invalid,         // Invalid data detected
};

// TCP stream framer: accumulates bytes and emits complete SOME/IP messages.
// Usage:
//   framer.feed(received_bytes);
//   while (auto msg = framer.next_message()) { ... }
class framer {
public:
    // Feed received data into the framer.
    void feed(std::span<const std::byte> data) {
        buf_.insert(buf_.end(), data.begin(), data.end());
    }

    // Try to extract the next complete message.
    // Returns the full message (header + payload) as a span,
    // or an empty span if not enough data.
    // The returned span is valid until the next call to feed() or next_message().
    std::span<const std::byte> next_message() {
        using namespace bsomeip::wire;

        // Consume processed bytes from previous call
        if (consumed_ > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + consumed_);
            consumed_ = 0;
        }

    rescan:
        if (buf_.size() < header_size) {
            return {}; // Need at least 16 bytes for a header
        }

        // Check for magic cookie (client: 0xDEAD/0xBEEF)
        // Magic cookie: service=0xFFFF/0xDEAD, method=0x0000/0xBEEF
        const_header_view hdr{buf_.data()};
        auto svc = hdr.service().value;
        auto mtd = hdr.method().value;

        // vsomeip magic cookies
        if ((svc == 0xFFFF && mtd == 0x0000) ||  // server cookie
            (svc == 0xDEAD && mtd == 0xBEEF)) {  // client cookie
            // Magic cookie is exactly 16 bytes with length=8
            auto len = hdr.length();
            if (len == wire::someip_header_size) {
                buf_.erase(buf_.begin(), buf_.begin() + header_size);
                goto rescan;
            }
        }

        // Read message length
        auto msg_size = hdr.message_size();
        if (msg_size > wire::max_tcp_message_size) {
            // Invalid length — skip one byte and rescan
            buf_.erase(buf_.begin(), buf_.begin() + 1);
            goto rescan;
        }

        if (buf_.size() < msg_size) {
            return {}; // Need more data
        }

        // Complete message available
        consumed_ = msg_size;
        return {buf_.data(), msg_size};
    }

    // Reset the framer state.
    void reset() {
        buf_.clear();
        consumed_ = 0;
    }

    // Bytes buffered but not yet consumed.
    std::size_t buffered() const noexcept { return buf_.size(); }

private:
    std::vector<std::byte> buf_;
    std::size_t consumed_ = 0;
};

} // namespace bsomeip::io
