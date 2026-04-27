// SPDX-License-Identifier: MIT
// Tests for bsomeip wire format: header, codec, TP segmentation.
// Compile with: -std=c++26 -freflection
#include <bsomeip/wire.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>

using namespace bsomeip::wire;

// ============================================================
// Test 1: Header value type — construct, read, write
// ============================================================
void test_header_basic() {
    auto hdr = header::make(
        service_id{0x1234}, method_id{0x5678},
        client_id{0x9ABC}, session_id{0xDEF0},
        0x01, message_type::request, return_code::e_ok);

    auto v = hdr.view();
    assert(v.service() == service_id{0x1234});
    assert(v.method() == method_id{0x5678});
    assert(v.client() == client_id{0x9ABC});
    assert(v.session() == session_id{0xDEF0});
    assert(v.protocol_ver() == 0x01);
    assert(v.interface_ver() == 0x01);
    assert(v.msg_type() == message_type::request);
    assert(v.ret_code() == return_code::e_ok);
    assert(v.payload_length() == 0);
    assert(v.message_size() == header_size);

    std::printf("[PASS] test_header_basic\n");
}

// ============================================================
// Test 2: Header big-endian wire format
// ============================================================
void test_header_wire_format() {
    auto hdr = header::make(
        service_id{0x1234}, method_id{0x5678},
        client_id{0x0001}, session_id{0x0002});

    // Verify big-endian encoding
    auto* b = reinterpret_cast<const unsigned char*>(hdr.bytes);
    // Service: 0x1234 → [0x12, 0x34]
    assert(b[0] == 0x12 && b[1] == 0x34);
    // Method: 0x5678 → [0x56, 0x78]
    assert(b[2] == 0x56 && b[3] == 0x78);
    // Client: 0x0001 → [0x00, 0x01]
    assert(b[8] == 0x00 && b[9] == 0x01);

    std::printf("[PASS] test_header_wire_format\n");
}

// ============================================================
// Test 3: Header payload length calculation
// ============================================================
void test_header_payload_length() {
    auto hdr = header::make(
        service_id{0x0001}, method_id{0x0001},
        client_id{0x0001}, session_id{0x0001});

    auto v = hdr.view();
    v.set_payload_length(100);
    assert(v.length() == 108); // 100 + 8
    assert(v.payload_length() == 100);
    assert(v.message_size() == 116); // 16 + 100

    std::printf("[PASS] test_header_payload_length\n");
}

// ============================================================
// Test 4: Strong ID types
// ============================================================
void test_strong_ids() {
    service_id a{0x1234};
    service_id b{0x1234};
    service_id c{0x5678};

    assert(a == b);
    assert(a != c);
    assert(a < c);
    assert(static_cast<std::uint16_t>(a) == 0x1234);

    // Different tag types are distinct (compile-time safety)
    // service_id s; method_id m = s; // Should not compile

    std::printf("[PASS] test_strong_ids\n");
}

// ============================================================
// Test 5: Message type helpers
// ============================================================
void test_message_type_helpers() {
    assert(is_request(message_type::request));
    assert(is_request(message_type::request_ack));
    assert(!is_request(message_type::response));
    assert(!is_request(message_type::notification));

    assert(is_response(message_type::response));
    assert(is_response(message_type::error));
    assert(!is_response(message_type::request));

    assert(!is_tp(message_type::request));
    assert(is_tp(with_tp(message_type::request)));
    assert(without_tp(with_tp(message_type::request)) == message_type::request);

    std::printf("[PASS] test_message_type_helpers\n");
}

// ============================================================
// Test 6: TP segmentation and reassembly
// ============================================================
void test_tp_segment_reassemble() {
    // Create a 3000-byte payload (will be split into segments)
    std::vector<std::byte> payload(3000);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    auto hdr = header::make(
        service_id{0x1234}, method_id{0x5678},
        client_id{0x0001}, session_id{0x0001});

    auto segments = segment_message(hdr, payload);
    assert(segments.size() >= 2); // 3000 / 1392 = 3 segments

    // Reassemble
    tp_assembler assembler;
    std::vector<std::byte> result;
    for (auto& seg : segments) {
        const_header_view seg_hdr{seg.data.data()};
        auto res = assembler.feed(seg_hdr, std::span{seg.data});
        if (res.has_value()) {
            result = std::move(*res);
        }
    }

    assert(result.size() == payload.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        assert(result[i] == payload[i]);
    }

    std::printf("[PASS] test_tp_segment_reassemble\n");
}

// ============================================================
// Test 7: Const header view
// ============================================================
void test_const_header_view() {
    auto hdr = header::make(
        service_id{0xAAAA}, method_id{0xBBBB},
        client_id{0xCCCC}, session_id{0xDDDD});

    const auto& chdr = hdr;
    auto cv = chdr.view();
    assert(cv.service() == service_id{0xAAAA});
    assert(cv.method() == method_id{0xBBBB});
    assert(cv.client() == client_id{0xCCCC});
    assert(cv.session() == session_id{0xDDDD});

    std::printf("[PASS] test_const_header_view\n");
}

// ============================================================
// Test 8: Constants sanity check
// ============================================================
void test_constants() {
    assert(header_size == 16);
    assert(someip_header_size == 8);
    assert(tp_header_size == 4);
    assert(payload_offset == 16);
    assert(tp_payload_offset == 20);
    assert(sd_service == service_id{0xFFFF});
    assert(sd_method == method_id{0x8100});
    assert(any_service == service_id{0xFFFF});

    std::printf("[PASS] test_constants\n");
}

// ============================================================
// Main
// ============================================================
int main() {
    test_header_basic();
    test_header_wire_format();
    test_header_payload_length();
    test_strong_ids();
    test_message_type_helpers();
    test_tp_segment_reassemble();
    test_const_header_view();
    test_constants();

    std::printf("\n=== All wire tests passed ===\n");
    return 0;
}
