// SPDX-License-Identifier: MIT
// Phase 5 tests: E2E protection profiles + Security policies.
// CRC round-trip, sender adaptors, policy evaluation, enforcer pipeline.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>

#include <stdexec/execution.hpp>

#include <bsomeip/e2e/crc.hpp>
#include <bsomeip/e2e/profile.hpp>
#include <bsomeip/e2e/protector.hpp>
#include <bsomeip/security/policy.hpp>
#include <bsomeip/security/enforcer.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>

namespace e2e = bsomeip::e2e;
namespace sec = bsomeip::security;
namespace wire = bsomeip::wire;

// ============================================================
// Helper: build a minimal SOME/IP message buffer
// ============================================================
std::vector<std::byte> make_message(wire::service_id service,
                                     wire::method_id method,
                                     wire::client_id client,
                                     wire::session_id session,
                                     std::size_t payload_size) {
    std::vector<std::byte> buf(wire::header_size + payload_size, std::byte{0});
    wire::header_view hdr{buf.data()};
    hdr.set_service(service);
    hdr.set_method(method);
    hdr.set_client(client);
    hdr.set_session(session);
    hdr.set_protocol_ver(wire::protocol_version);
    hdr.set_msg_type(wire::message_type::request);
    hdr.set_ret_code(wire::return_code::e_ok);
    hdr.set_payload_length(static_cast<wire::length_t>(payload_size));
    return buf;
}

// ============================================================
// Test 1: CRC-8/SAE-J1850 known vector
// ============================================================
void test_crc8() {
    // Known test: "123456789" → CRC-8/SAE-J1850 = 0x4B
    std::array<std::byte, 9> data;
    const char* input = "123456789";
    for (int i = 0; i < 9; ++i)
        data[i] = static_cast<std::byte>(input[i]);

    auto crc = e2e::crc8_sae_j1850(data);
    assert(crc == 0x4B);

    std::printf("[PASS] test_crc8\n");
}

// ============================================================
// Test 2: CRC-32/Ethernet known vector
// ============================================================
void test_crc32() {
    // Known test: "123456789" → CRC-32 = 0xCBF43926
    std::array<std::byte, 9> data;
    const char* input = "123456789";
    for (int i = 0; i < 9; ++i)
        data[i] = static_cast<std::byte>(input[i]);

    auto crc = e2e::crc32_ethernet(data);
    assert(crc == 0xCBF43926u);

    std::printf("[PASS] test_crc32\n");
}

// ============================================================
// Test 3: CRC-64 basic test
// ============================================================
void test_crc64() {
    std::array<std::byte, 4> data{
        std::byte{0x01}, std::byte{0x02},
        std::byte{0x03}, std::byte{0x04}};

    auto crc1 = e2e::crc64(data);
    auto crc2 = e2e::crc64(data);
    assert(crc1 == crc2);  // deterministic
    assert(crc1 != 0);     // non-trivial

    // Different data → different CRC
    data[0] = std::byte{0xFF};
    auto crc3 = e2e::crc64(data);
    assert(crc3 != crc1);

    std::printf("[PASS] test_crc64\n");
}

// ============================================================
// Test 4: Profile 01 protect + check round-trip
// ============================================================
void test_profile_01() {
    e2e::profile_01_config cfg{.data_id = 0x1234, .max_delta_counter = 2};

    // Create a payload with E2E region (at least 2 bytes for P01)
    std::array<std::byte, 8> payload{};
    payload[2] = std::byte{0xAA};  // user data
    payload[3] = std::byte{0xBB};

    // Protect with counter=1
    e2e::profile_01::protect(payload, cfg, 1);

    // CRC byte should be set
    assert(payload[0] != std::byte{0} || payload[1] != std::byte{0});

    // Check should pass
    std::uint8_t last_counter = 0;
    auto result = e2e::profile_01::check(payload, cfg, last_counter);
    assert(result == e2e::e2e_result::ok);
    assert(last_counter == 1);

    // Same counter again → repeated
    result = e2e::profile_01::check(payload, cfg, last_counter);
    assert(result == e2e::e2e_result::repeated);

    // Corrupt a byte → wrong CRC
    payload[3] ^= std::byte{0xFF};
    last_counter = 0;
    result = e2e::profile_01::check(payload, cfg, last_counter);
    assert(result == e2e::e2e_result::wrong_crc);

    std::printf("[PASS] test_profile_01\n");
}

// ============================================================
// Test 5: Profile 04 protect + check round-trip
// ============================================================
void test_profile_04() {
    e2e::profile_04_config cfg{.data_id = 0xDEADBEEF, .max_delta_counter = 1};

    // Payload: 12 bytes header + 8 bytes user data
    std::vector<std::byte> payload(20, std::byte{0});
    payload[12] = std::byte{0xCA};
    payload[13] = std::byte{0xFE};

    e2e::profile_04::protect(payload, cfg, 1);

    // Verify length field was written
    auto len = e2e::profile_04::read_u16_be(payload.data());
    assert(len == 20);

    // Verify data_id was written
    auto did = e2e::profile_04::read_u32_be(payload.data() + 4);
    assert(did == 0xDEADBEEF);

    // Check
    std::uint16_t last_counter = 0;
    auto result = e2e::profile_04::check(payload, cfg, last_counter);
    assert(result == e2e::e2e_result::ok);
    assert(last_counter == 1);

    // Corrupt → wrong CRC
    payload[15] ^= std::byte{0x01};
    last_counter = 0;
    result = e2e::profile_04::check(payload, cfg, last_counter);
    assert(result == e2e::e2e_result::wrong_crc);

    std::printf("[PASS] test_profile_04\n");
}

// ============================================================
// Test 6: Profile 07 protect + check round-trip
// ============================================================
void test_profile_07() {
    e2e::profile_07_config cfg{.data_id = 0x12345678, .max_delta_counter = 1};

    // Payload: 20 bytes header + 4 bytes user data
    std::vector<std::byte> payload(24, std::byte{0});
    payload[20] = std::byte{0xDE};
    payload[21] = std::byte{0xAD};

    e2e::profile_07::protect(payload, cfg, 1);

    // Verify fields
    auto rlen = e2e::profile_07::read_u32_be(payload.data());
    assert(rlen == 24);

    std::uint32_t last_counter = 0;
    auto result = e2e::profile_07::check(payload, cfg, last_counter);
    assert(result == e2e::e2e_result::ok);
    assert(last_counter == 1);

    std::printf("[PASS] test_profile_07\n");
}

// ============================================================
// Test 7: E2E protect sender adaptor + sync_wait
// ============================================================
void test_protect_sender() {
    e2e::profile_04_config cfg{.data_id = 0xAABBCCDD, .max_delta_counter = 1};
    e2e::e2e_state<e2e::profile_04> state{cfg};

    // Create a message with a payload large enough for P04 header (12 bytes)
    auto msg = make_message(wire::service_id{0x1234}, wire::method_id{0x0001},
                            wire::client_id{0x0010}, wire::session_id{1}, 20);

    // Use sender pipeline: just(msg) | e2e::protect(state) | sync_wait
    auto result = stdexec::sync_wait(
        stdexec::just(std::move(msg))
        | e2e::protect(state)
    );

    assert(result.has_value());
    auto& [protected_msg] = *result;
    assert(protected_msg.size() == wire::header_size + 20);

    // The payload should now have E2E P04 header
    auto payload = std::span{protected_msg}.subspan(wire::header_size);
    auto len = e2e::profile_04::read_u16_be(payload.data());
    assert(len == 20);

    std::printf("[PASS] test_protect_sender\n");
}

// ============================================================
// Test 8: E2E protect + check round-trip via sender pipeline
// ============================================================
void test_protect_check_pipeline() {
    e2e::profile_04_config cfg{.data_id = 0x11223344, .max_delta_counter = 2};
    e2e::e2e_state<e2e::profile_04> protect_state{cfg};
    e2e::e2e_state<e2e::profile_04> check_state{cfg};

    auto msg = make_message(wire::service_id{0x1234}, wire::method_id{0x0001},
                            wire::client_id{0x0010}, wire::session_id{1}, 20);

    // protect → check pipeline should succeed
    auto result = stdexec::sync_wait(
        stdexec::just(std::move(msg))
        | e2e::protect(protect_state)
        | e2e::check(check_state)
    );

    assert(result.has_value());
    auto& [verified_msg] = *result;
    assert(verified_msg.size() == wire::header_size + 20);

    std::printf("[PASS] test_protect_check_pipeline\n");
}

// ============================================================
// Test 9: E2E check sender rejects corrupted message
// ============================================================
void test_check_rejects_corrupt() {
    e2e::profile_04_config cfg{.data_id = 0x55667788, .max_delta_counter = 1};
    e2e::e2e_state<e2e::profile_04> protect_state{cfg};
    e2e::e2e_state<e2e::profile_04> check_state{cfg};

    auto msg = make_message(wire::service_id{0x1234}, wire::method_id{0x0001},
                            wire::client_id{0x0010}, wire::session_id{1}, 20);

    // Protect it
    auto protected_result = stdexec::sync_wait(
        stdexec::just(std::move(msg))
        | e2e::protect(protect_state)
    );
    assert(protected_result.has_value());
    auto corrupted = std::get<0>(*protected_result);

    // Corrupt a byte in the payload
    corrupted[wire::header_size + 15] ^= std::byte{0xFF};

    // Check should fail
    bool got_error = false;
    try {
        auto check_result = stdexec::sync_wait(
            stdexec::just(std::move(corrupted))
            | e2e::check(check_state)
        );
        // sync_wait returns optional — if the sender completed with error,
        // sync_wait re-throws
        (void)check_result;
    } catch (const e2e::e2e_error& e) {
        got_error = true;
        assert(e.result() == e2e::e2e_result::wrong_crc);
    } catch (...) {
        got_error = true; // any error is acceptable
    }
    assert(got_error);

    std::printf("[PASS] test_check_rejects_corrupt\n");
}

// ============================================================
// Test 10: Security policy — basic allow/deny
// ============================================================
void test_policy_basic() {
    sec::policy pol;

    // Allow client 0x0010 to access service 0x1234
    pol.add(sec::allow_client_service(
        wire::client_id{0x0010}, wire::service_id{0x1234}));

    // Deny everything else (default)

    assert(pol.is_allowed(wire::client_id{0x0010}, wire::service_id{0x1234},
                          wire::instance_id{1}, wire::method_id{1}));

    // Different client → denied
    assert(!pol.is_allowed(wire::client_id{0x0020}, wire::service_id{0x1234},
                           wire::instance_id{1}, wire::method_id{1}));

    // Different service → denied
    assert(!pol.is_allowed(wire::client_id{0x0010}, wire::service_id{0x5678},
                           wire::instance_id{1}, wire::method_id{1}));

    std::printf("[PASS] test_policy_basic\n");
}

// ============================================================
// Test 11: Security policy — wildcard rules and order
// ============================================================
void test_policy_wildcards() {
    sec::policy pol;

    // Deny client 0x0099 specifically
    pol.add(sec::deny_client(wire::client_id{0x0099}));

    // Allow all others
    pol.add(sec::allow_all());

    // Client 0x0099 → denied (first match wins)
    assert(!pol.is_allowed(wire::client_id{0x0099}, wire::service_id{0x1234},
                           wire::instance_id{1}, wire::method_id{1}));

    // Any other client → allowed
    assert(pol.is_allowed(wire::client_id{0x0010}, wire::service_id{0x1234},
                          wire::instance_id{1}, wire::method_id{1}));
    assert(pol.is_allowed(wire::client_id{0x0001}, wire::service_id{0x5678},
                          wire::instance_id{2}, wire::method_id{3}));

    std::printf("[PASS] test_policy_wildcards\n");
}

// ============================================================
// Test 12: Security policy — method-level granularity
// ============================================================
void test_policy_method_level() {
    sec::policy pol;

    // Only allow method 0x0001 on service 0x1234
    pol.add(sec::allow_service_method(
        wire::service_id{0x1234}, wire::method_id{0x0001}));

    assert(pol.is_allowed(wire::client_id{0x0010}, wire::service_id{0x1234},
                          wire::instance_id{1}, wire::method_id{0x0001}));

    // Different method → denied
    assert(!pol.is_allowed(wire::client_id{0x0010}, wire::service_id{0x1234},
                           wire::instance_id{1}, wire::method_id{0x0002}));

    std::printf("[PASS] test_policy_method_level\n");
}

// ============================================================
// Test 13: Security enforcer sender — allowed message
// ============================================================
void test_enforcer_allows() {
    sec::policy pol;
    pol.add(sec::allow_all());

    auto msg = make_message(wire::service_id{0x1234}, wire::method_id{0x0001},
                            wire::client_id{0x0010}, wire::session_id{1}, 8);

    auto result = stdexec::sync_wait(
        stdexec::just(std::move(msg))
        | sec::enforce(pol)
    );

    assert(result.has_value());
    auto& [passed_msg] = *result;
    assert(passed_msg.size() == wire::header_size + 8);

    std::printf("[PASS] test_enforcer_allows\n");
}

// ============================================================
// Test 14: Security enforcer sender — denied message
// ============================================================
void test_enforcer_denies() {
    sec::policy pol;
    // Only allow client 0x0010
    pol.add(sec::allow_client_service(
        wire::client_id{0x0010}, wire::service_id{0x1234}));

    // Message from client 0x0099 → should be denied
    auto msg = make_message(wire::service_id{0x1234}, wire::method_id{0x0001},
                            wire::client_id{0x0099}, wire::session_id{1}, 8);

    bool got_error = false;
    try {
        auto result = stdexec::sync_wait(
            stdexec::just(std::move(msg))
            | sec::enforce(pol)
        );
        (void)result;
    } catch (const sec::security_error& e) {
        got_error = true;
        assert(e.client() == wire::client_id{0x0099});
        assert(e.service() == wire::service_id{0x1234});
    } catch (...) {
        got_error = true;
    }
    assert(got_error);

    std::printf("[PASS] test_enforcer_denies\n");
}

// ============================================================
// Test 15: Combined pipeline — enforce + e2e protect
// ============================================================
void test_combined_pipeline() {
    // Security policy
    sec::policy pol;
    pol.add(sec::allow_all());

    // E2E state
    e2e::profile_04_config cfg{.data_id = 0xFACE, .max_delta_counter = 1};
    e2e::e2e_state<e2e::profile_04> prot_state{cfg};
    e2e::e2e_state<e2e::profile_04> chk_state{cfg};

    auto msg = make_message(wire::service_id{0x1234}, wire::method_id{0x0001},
                            wire::client_id{0x0010}, wire::session_id{1}, 20);

    // Full pipeline: enforce → protect → check
    auto result = stdexec::sync_wait(
        stdexec::just(std::move(msg))
        | sec::enforce(pol)
        | e2e::protect(prot_state)
        | e2e::check(chk_state)
    );

    assert(result.has_value());
    auto& [final_msg] = *result;
    assert(final_msg.size() == wire::header_size + 20);

    std::printf("[PASS] test_combined_pipeline\n");
}

// ============================================================
// Test 16: Counter sequence detection
// ============================================================
void test_counter_sequence() {
    e2e::profile_04_config cfg{.data_id = 0xAAAA, .max_delta_counter = 2};

    auto make_payload = [&](std::uint16_t counter) {
        std::vector<std::byte> payload(16, std::byte{0});
        e2e::profile_04::protect(payload, cfg, counter);
        return payload;
    };

    std::uint16_t last = 0;

    // Counter 1 → ok (delta=1, ≤2)
    auto p1 = make_payload(1);
    assert(e2e::profile_04::check(p1, cfg, last) == e2e::e2e_result::ok);

    // Counter 2 → ok (delta=1)
    auto p2 = make_payload(2);
    assert(e2e::profile_04::check(p2, cfg, last) == e2e::e2e_result::ok);

    // Counter 2 again → repeated (delta=0)
    auto p2b = make_payload(2);
    assert(e2e::profile_04::check(p2b, cfg, last) == e2e::e2e_result::repeated);

    // Counter 6 → wrong_sequence (delta=4, >2)
    auto p6 = make_payload(6);
    assert(e2e::profile_04::check(p6, cfg, last) == e2e::e2e_result::wrong_sequence);

    std::printf("[PASS] test_counter_sequence\n");
}

int main() {
    // CRC tests
    test_crc8();
    test_crc32();
    test_crc64();

    // Profile tests
    test_profile_01();
    test_profile_04();
    test_profile_07();

    // Sender adaptor tests
    test_protect_sender();
    test_protect_check_pipeline();
    test_check_rejects_corrupt();

    // Security policy tests
    test_policy_basic();
    test_policy_wildcards();
    test_policy_method_level();

    // Security enforcer sender tests
    test_enforcer_allows();
    test_enforcer_denies();

    // Combined pipeline
    test_combined_pipeline();

    // Counter sequence
    test_counter_sequence();

    std::printf("\n=== All E2E + Security tests passed ===\n");
    return 0;
}
