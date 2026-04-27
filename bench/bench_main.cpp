// SPDX-License-Identifier: MIT
// bsomeip performance benchmarks.
//
// Measures throughput for:
//   1. Wire codec (serialize/deserialize) — reflection-based
//   2. CRC computation (CRC-8, CRC-32, CRC-64)
//   3. E2E protect/check (Profile 04)
//   4. Message routing/dispatch (in-process)
//   5. Full sender pipeline (async_call: serialize → route → dispatch → deserialize)
//   6. Security policy evaluation
//   7. Combined pipeline (enforce + e2e protect + check)
//
// No external dependencies — uses rdtsc + chrono for timing.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>

#include <stdexec/execution.hpp>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/e2e/crc.hpp>
#include <bsomeip/e2e/profile.hpp>
#include <bsomeip/e2e/protector.hpp>
#include <bsomeip/security/policy.hpp>
#include <bsomeip/security/enforcer.hpp>
#include <bsomeip/route/dispatcher.hpp>
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/skeleton.hpp>
#include <bsomeip/api/proxy.hpp>

namespace wire = bsomeip::wire;
namespace e2e = bsomeip::e2e;
namespace sec = bsomeip::security;
namespace api = bsomeip::api;
namespace route = bsomeip::route;

// ============================================================
// Timing helpers
// ============================================================

using bench_clock = std::chrono::high_resolution_clock;

struct bench_result {
    const char* name;
    std::uint64_t iterations;
    double elapsed_ns;

    double ns_per_op() const { return elapsed_ns / iterations; }
    double ops_per_sec() const { return iterations / (elapsed_ns * 1e-9); }
};

void print_result(const bench_result& r) {
    std::printf("  %-42s  %10.1f ns/op  %12.0f ops/sec  (%lu iters)\n",
                r.name, r.ns_per_op(), r.ops_per_sec(),
                static_cast<unsigned long>(r.iterations));
}

template <typename Fn>
bench_result benchmark(const char* name, std::uint64_t iters, Fn&& fn) {
    // Warmup
    for (std::uint64_t i = 0; i < std::min(iters / 10, std::uint64_t{1000}); ++i)
        fn();

    auto start = bench_clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
        fn();
    }
    auto end = bench_clock::now();

    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return {name, iters, ns};
}

// Escape pointer so optimizer doesn't elide our work.
static void do_not_optimize(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

// ============================================================
// Test payloads — small/medium/large
// ============================================================

// Small: 8 bytes (typical sensor value)
struct small_payload { std::int32_t a; std::int32_t b; };

// Medium: 64 bytes (typical structured message)
struct medium_payload {
    std::uint32_t id;
    std::int32_t x; std::int32_t y; std::int32_t z;
    float temperature;
    float pressure;
    float humidity;
    std::uint64_t timestamp;
    std::uint32_t flags;
    std::int16_t quality;
    std::int16_t reserved1;
    std::uint32_t sequence;
    std::uint32_t checksum;
};

// Large: ~248 bytes (many scalar fields)
struct large_payload {
    std::uint32_t count;
    std::uint32_t type;
    std::int32_t v0; std::int32_t v1; std::int32_t v2; std::int32_t v3;
    std::int32_t v4; std::int32_t v5; std::int32_t v6; std::int32_t v7;
    std::int32_t v8; std::int32_t v9; std::int32_t v10; std::int32_t v11;
    std::int32_t v12; std::int32_t v13; std::int32_t v14; std::int32_t v15;
    std::int32_t v16; std::int32_t v17; std::int32_t v18; std::int32_t v19;
    std::int32_t v20; std::int32_t v21; std::int32_t v22; std::int32_t v23;
    std::int32_t v24; std::int32_t v25; std::int32_t v26; std::int32_t v27;
    std::int32_t v28; std::int32_t v29;
    std::uint32_t crc;
};

// ============================================================
// 1. Wire codec benchmarks
// ============================================================

void bench_codec() {
    std::printf("\n--- Wire Codec (reflection-based serialize/deserialize) ---\n");

    constexpr std::uint64_t N = 1'000'000;

    // Small payload
    {
        small_payload src{42, -100};
        std::array<std::byte, 64> buf{};
        small_payload dst{};

        auto r1 = benchmark("serialize small (8B)", N, [&] {
            auto r = wire::serialize(std::span{buf}, src);
            do_not_optimize(buf.data());
        });
        print_result(r1);

        wire::serialize(std::span{buf}, src);
        auto r2 = benchmark("deserialize small (8B)", N, [&] {
            auto r = wire::deserialize<small_payload>(std::span<const std::byte>{buf});
            do_not_optimize(&r);
        });
        print_result(r2);
    }

    // Medium payload
    {
        medium_payload src{1, 100, 200, 300, 36.5f, 1013.25f, 65.0f,
                           1234567890ULL, 0xFF, 90, 0, 42, 0xDEAD};
        std::array<std::byte, 256> buf{};

        auto r1 = benchmark("serialize medium (52B)", N, [&] {
            auto r = wire::serialize(std::span{buf}, src);
            do_not_optimize(buf.data());
        });
        print_result(r1);

        wire::serialize(std::span{buf}, src);
        auto r2 = benchmark("deserialize medium (52B)", N, [&] {
            auto r = wire::deserialize<medium_payload>(std::span<const std::byte>{buf});
            do_not_optimize(&r);
        });
        print_result(r2);
    }

    // Large payload
    {
        large_payload src{};
        src.count = 30; src.type = 1; src.crc = 0xCAFE;
        src.v0 = 100; src.v5 = 500; src.v10 = 1000; src.v20 = 2000;
        std::array<std::byte, 512> buf{};

        auto r1 = benchmark("serialize large (132B)", N, [&] {
            auto r = wire::serialize(std::span{buf}, src);
            do_not_optimize(buf.data());
        });
        print_result(r1);

        wire::serialize(std::span{buf}, src);
        auto r2 = benchmark("deserialize large (132B)", N, [&] {
            auto r = wire::deserialize<large_payload>(std::span<const std::byte>{buf});
            do_not_optimize(&r);
        });
        print_result(r2);
    }
}

// ============================================================
// 2. CRC benchmarks
// ============================================================

void bench_crc() {
    std::printf("\n--- CRC Computation ---\n");

    constexpr std::uint64_t N = 1'000'000;

    // 64-byte payload
    std::array<std::byte, 64> data{};
    for (int i = 0; i < 64; ++i) data[i] = static_cast<std::byte>(i);

    auto r1 = benchmark("CRC-8/SAE-J1850 (64B)", N, [&] {
        auto c = e2e::crc8_sae_j1850(data);
        do_not_optimize(&c);
    });
    print_result(r1);

    auto r2 = benchmark("CRC-32/Ethernet (64B)", N, [&] {
        auto c = e2e::crc32_ethernet(data);
        do_not_optimize(&c);
    });
    print_result(r2);

    auto r3 = benchmark("CRC-64 (64B)", N, [&] {
        auto c = e2e::crc64(data);
        do_not_optimize(&c);
    });
    print_result(r3);

    // 1KB payload
    std::array<std::byte, 1024> big_data{};
    for (int i = 0; i < 1024; ++i) big_data[i] = static_cast<std::byte>(i & 0xFF);

    auto r4 = benchmark("CRC-32/Ethernet (1KB)", N, [&] {
        auto c = e2e::crc32_ethernet(big_data);
        do_not_optimize(&c);
    });
    print_result(r4);

    auto r5 = benchmark("CRC-64 (1KB)", N, [&] {
        auto c = e2e::crc64(big_data);
        do_not_optimize(&c);
    });
    print_result(r5);
}

// ============================================================
// 3. E2E profile protect/check
// ============================================================

void bench_e2e() {
    std::printf("\n--- E2E Profile 04 (CRC-32, 16-bit counter) ---\n");

    constexpr std::uint64_t N = 1'000'000;

    e2e::profile_04_config cfg{.data_id = 0xDEADBEEF, .max_delta_counter = 100};

    // 64-byte payload (12 header + 52 data)
    std::vector<std::byte> payload(64, std::byte{0xAA});
    std::uint16_t counter = 0;

    auto r1 = benchmark("P04 protect (64B)", N, [&] {
        e2e::profile_04::protect(payload, cfg, counter++);
        do_not_optimize(payload.data());
    });
    print_result(r1);

    // Reset for check
    counter = 0;
    e2e::profile_04::protect(payload, cfg, 1);
    std::uint16_t last = 0;
    std::uint16_t check_counter = 1;

    auto r2 = benchmark("P04 check (64B)", N, [&] {
        // Re-protect with incrementing counter to avoid wrong_sequence
        e2e::profile_04::protect(payload, cfg, check_counter);
        auto res = e2e::profile_04::check(payload, cfg, last);
        do_not_optimize(&res);
        check_counter++;
    });
    print_result(r2);
}

// ============================================================
// 4. Message dispatch (in-process routing)
// ============================================================

void bench_dispatch() {
    std::printf("\n--- Message Dispatch ---\n");

    constexpr std::uint64_t N = 1'000'000;

    route::dispatcher disp;

    volatile int handler_count = 0;
    disp.register_handler(wire::service_id{0x1234}, wire::method_id{0x0001},
        [&](const route::message_view&) { handler_count++; });

    // Build a request message
    std::array<std::byte, 24> msg_buf{};
    wire::header_view hdr{msg_buf.data()};
    hdr.set_service(wire::service_id{0x1234});
    hdr.set_method(wire::method_id{0x0001});
    hdr.set_client(wire::client_id{0x0010});
    hdr.set_session(wire::session_id{1});
    hdr.set_protocol_ver(wire::protocol_version);
    hdr.set_msg_type(wire::message_type::request);
    hdr.set_payload_length(8);

    auto payload = std::span{msg_buf}.subspan(wire::header_size);

    auto r = benchmark("dispatch (service,method) lookup", N, [&] {
        disp.dispatch(hdr, payload);
    });
    print_result(r);
}

// ============================================================
// 5. Full sender pipeline: proxy async_call → skeleton serve
// ============================================================

void bench_async_call() {
    std::printf("\n--- Sender Pipeline (async_call round-trip, in-process) ---\n");

    constexpr std::uint64_t N = 500'000;

    api::application app;

    struct calc_impl {
        small_payload add(const small_payload& req) {
            return {req.a + req.b, 0};
        }
    };

    calc_impl impl;
    api::skeleton<calc_impl> skel(app, impl);
    skel.offer(wire::service_id{0x1234}, wire::instance_id{0x0001}, 1, 0);
    skel.serve<small_payload, small_payload>(
        wire::method_id{0x0001},
        [](calc_impl& s, const small_payload& r) { return s.add(r); });

    api::proxy<> prx(app);
    prx.target(wire::service_id{0x1234}, wire::instance_id{0x0001});

    small_payload req{10, 20};

    auto r = benchmark("async_call + sync_wait (8B)", N, [&] {
        auto result = stdexec::sync_wait(
            prx.async_call<small_payload, small_payload>(
                wire::method_id{0x0001}, req));
        do_not_optimize(&result);
    });
    print_result(r);

    // With then() composition
    auto r2 = benchmark("async_call | then() | sync_wait", N, [&] {
        auto result = stdexec::sync_wait(
            prx.async_call<small_payload, small_payload>(
                wire::method_id{0x0001}, req)
            | stdexec::then([](small_payload resp) -> int {
                return resp.a;
            }));
        do_not_optimize(&result);
    });
    print_result(r2);
}

// ============================================================
// 6. Security policy evaluation
// ============================================================

void bench_security() {
    std::printf("\n--- Security Policy ---\n");

    constexpr std::uint64_t N = 5'000'000;

    // Small policy: 5 rules
    sec::policy small_pol;
    small_pol.add(sec::allow_client_service(wire::client_id{0x0001}, wire::service_id{0x1234}));
    small_pol.add(sec::allow_client_service(wire::client_id{0x0002}, wire::service_id{0x1234}));
    small_pol.add(sec::allow_client_service(wire::client_id{0x0003}, wire::service_id{0x5678}));
    small_pol.add(sec::deny_client(wire::client_id{0x00FF}));
    small_pol.add(sec::allow_all());

    auto r1 = benchmark("policy eval (5 rules, hit #1)", N, [&] {
        auto a = small_pol.evaluate(wire::client_id{0x0001}, wire::service_id{0x1234},
                                    wire::instance_id{1}, wire::method_id{1});
        do_not_optimize(&a);
    });
    print_result(r1);

    auto r2 = benchmark("policy eval (5 rules, hit #5)", N, [&] {
        auto a = small_pol.evaluate(wire::client_id{0x9999}, wire::service_id{0x9999},
                                    wire::instance_id{1}, wire::method_id{1});
        do_not_optimize(&a);
    });
    print_result(r2);

    // Larger policy: 100 rules
    sec::policy big_pol;
    for (int i = 0; i < 99; ++i) {
        big_pol.add(sec::allow_client_service(
            wire::client_id{static_cast<std::uint16_t>(i + 1)},
            wire::service_id{static_cast<std::uint16_t>(i + 0x100)}));
    }
    big_pol.add(sec::allow_all());

    auto r3 = benchmark("policy eval (100 rules, hit last)", N, [&] {
        auto a = big_pol.evaluate(wire::client_id{0xFFFF}, wire::service_id{0xFFFF},
                                  wire::instance_id{1}, wire::method_id{1});
        do_not_optimize(&a);
    });
    print_result(r3);
}

// ============================================================
// 7. Combined sender pipeline: enforce + e2e protect
// ============================================================

void bench_combined() {
    std::printf("\n--- Combined Pipeline (enforce + e2e protect + check) ---\n");

    constexpr std::uint64_t N = 500'000;

    sec::policy pol;
    pol.add(sec::allow_all());

    e2e::profile_04_config cfg{.data_id = 0xFACE, .max_delta_counter = 60000};
    e2e::e2e_state<e2e::profile_04> prot_state{cfg};
    e2e::e2e_state<e2e::profile_04> chk_state{cfg};

    // Pre-build a message (header + 20 bytes payload for P04)
    auto make_msg = [] {
        std::vector<std::byte> buf(wire::header_size + 20, std::byte{0});
        wire::header_view hdr{buf.data()};
        hdr.set_service(wire::service_id{0x1234});
        hdr.set_method(wire::method_id{0x0001});
        hdr.set_client(wire::client_id{0x0010});
        hdr.set_session(wire::session_id{1});
        hdr.set_protocol_ver(wire::protocol_version);
        hdr.set_msg_type(wire::message_type::request);
        hdr.set_payload_length(20);
        return buf;
    };

    auto r = benchmark("enforce | protect | check (20B payload)", N, [&] {
        auto msg = make_msg();
        auto result = stdexec::sync_wait(
            stdexec::just(std::move(msg))
            | sec::enforce(pol)
            | e2e::protect(prot_state)
            | e2e::check(chk_state)
        );
        do_not_optimize(&result);
    });
    print_result(r);
}

// ============================================================
// 8. Header read/write throughput
// ============================================================

void bench_header() {
    std::printf("\n--- Header Read/Write ---\n");

    constexpr std::uint64_t N = 5'000'000;

    std::array<std::byte, 16> buf{};
    wire::header_view hdr{buf.data()};

    auto r1 = benchmark("header write (all fields)", N, [&] {
        hdr.set_service(wire::service_id{0x1234});
        hdr.set_method(wire::method_id{0x0001});
        hdr.set_client(wire::client_id{0x0010});
        hdr.set_session(wire::session_id{42});
        hdr.set_protocol_ver(1);
        hdr.set_interface_ver(1);
        hdr.set_msg_type(wire::message_type::request);
        hdr.set_ret_code(wire::return_code::e_ok);
        hdr.set_payload_length(100);
        do_not_optimize(buf.data());
    });
    print_result(r1);

    auto r2 = benchmark("header read (all fields)", N, [&] {
        auto s = hdr.service();
        auto m = hdr.method();
        auto c = hdr.client();
        auto ss = hdr.session();
        auto mt = hdr.msg_type();
        auto rc = hdr.ret_code();
        auto pl = hdr.payload_length();
        do_not_optimize(&s);
        do_not_optimize(&pl);
    });
    print_result(r2);
}

// ============================================================
// main
// ============================================================

int main() {
    std::printf("=== bsomeip Performance Benchmarks ===\n");
    std::printf("Each benchmark runs a fixed number of iterations.\n");
    std::printf("Lower ns/op = better. Higher ops/sec = better.\n");

    bench_header();
    bench_codec();
    bench_crc();
    bench_e2e();
    bench_dispatch();
    bench_async_call();
    bench_security();
    bench_combined();

    std::printf("\n=== Benchmark complete ===\n");
    return 0;
}
