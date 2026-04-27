// SPDX-License-Identifier: MIT
// SOME/IP-TP (Transport Protocol) segmentation and reassembly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <map>
#include <expected>

#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>

namespace bsomeip::wire {

// TP header: 4 bytes after the SOME/IP header
// Bits 0-27:  offset (in bytes, of this segment in the reassembled payload)
// Bits 28-30: reserved (0)
// Bit 31:     more_segments (1 = more to come, 0 = last segment)
// TODO(bitfield-layout): Bitfield layout is implementation-defined. We manually
// parse/write via read_tp_header()/write_tp_header() to guarantee the wire
// format.  This struct is only used as an in-memory convenience type.
struct tp_header {
    std::uint32_t offset       : 28 = 0;
    std::uint32_t reserved     : 3  = 0;
    std::uint32_t more_segments : 1  = 0;
};

static_assert(sizeof(tp_header) == 4);

constexpr tp_header read_tp_header(const std::byte* p) noexcept {
    auto raw = read_u32_be(p);
    tp_header h;
    h.offset = raw >> 4;
    h.reserved = 0;
    h.more_segments = raw & 1;
    return h;
}

constexpr void write_tp_header(std::byte* p, const tp_header& h) noexcept {
    std::uint32_t raw = (h.offset << 4) | (h.more_segments & 1);
    write_u32_be(p, raw);
}

// --- Segmentation: split large payload into TP segments ---

struct segment {
    std::vector<std::byte> data; // header (16) + tp_header (4) + segment_payload
};

// Split a message into TP segments.
// Input: full SOME/IP header + payload.
// Output: vector of segments, each with TP flag set and TP header inserted.
inline std::vector<segment> segment_message(
    const header& hdr,
    std::span<const std::byte> payload,
    std::size_t max_segment_payload = tp_max_segment_length) noexcept
{
    std::vector<segment> segments;
    std::size_t offset = 0;

    while (offset < payload.size()) {
        std::size_t remaining = payload.size() - offset;
        std::size_t chunk = remaining > max_segment_payload ? max_segment_payload : remaining;
        bool more = (offset + chunk) < payload.size();

        segment seg;
        seg.data.resize(header_size + tp_header_size + chunk);

        // Copy and modify header
        for (std::size_t i = 0; i < header_size; ++i) {
            seg.data[i] = hdr.bytes[i];
        }
        header_view seg_hdr{seg.data.data()};
        seg_hdr.set_msg_type(with_tp(seg_hdr.msg_type()));
        // Length = 8 (protocol fields) + 4 (TP header) + chunk
        seg_hdr.set_length(static_cast<length_t>(someip_header_size + tp_header_size + chunk));

        // Write TP header
        tp_header tph;
        tph.offset = static_cast<std::uint32_t>(offset);
        tph.more_segments = more ? 1 : 0;
        write_tp_header(seg.data.data() + header_size, tph);

        // Copy payload chunk
        auto src = payload.data() + offset;
        auto dst = seg.data.data() + header_size + tp_header_size;
        for (std::size_t i = 0; i < chunk; ++i) {
            dst[i] = src[i];
        }

        segments.push_back(std::move(seg));
        offset += chunk;
    }

    return segments;
}

// --- Reassembly: collect TP segments into full payload ---

// Key for identifying a TP message stream
struct tp_stream_key {
    std::uint16_t service;
    std::uint16_t method;
    std::uint16_t client;
    std::uint16_t session;

    auto operator<=>(const tp_stream_key&) const = default;
};

class tp_assembler {
public:
    // Feed a received segment. Returns the reassembled payload when complete.
    std::expected<std::vector<std::byte>, codec_error>
    feed(const_header_view hdr, std::span<const std::byte> segment_data) {
        tp_stream_key key{
            hdr.service().value,
            hdr.method().value,
            hdr.client().value,
            hdr.session().value,
        };

        // Read TP header (immediately after SOME/IP header)
        if (segment_data.size() < header_size + tp_header_size) {
            return std::unexpected{codec_error::buffer_too_small};
        }
        auto tph = read_tp_header(segment_data.data() + header_size);

        auto& stream = streams_[key];
        auto payload_data = segment_data.subspan(header_size + tp_header_size);
        stream.fragments[tph.offset] = {payload_data.begin(), payload_data.end()};

        if (tph.more_segments == 0) {
            stream.final_offset = tph.offset + static_cast<std::uint32_t>(payload_data.size());
        }

        // Check if all fragments are received
        if (stream.final_offset == 0) {
            return std::unexpected{codec_error::invalid_length}; // Not yet complete
        }

        // Try to assemble
        std::vector<std::byte> result;
        result.resize(stream.final_offset);
        std::uint32_t expected = 0;
        for (auto& [off, frag] : stream.fragments) {
            if (off != expected) {
                return std::unexpected{codec_error::invalid_length}; // Gap
            }
            for (std::size_t i = 0; i < frag.size(); ++i) {
                result[off + i] = frag[i];
            }
            expected = off + static_cast<std::uint32_t>(frag.size());
        }
        if (expected != stream.final_offset) {
            return std::unexpected{codec_error::invalid_length}; // Incomplete
        }

        streams_.erase(key);
        return result;
    }

    void clear() { streams_.clear(); }

private:
    struct stream_state {
        std::map<std::uint32_t, std::vector<std::byte>> fragments;
        std::uint32_t final_offset = 0;
    };
    std::map<tp_stream_key, stream_state> streams_;
};

} // namespace bsomeip::wire
