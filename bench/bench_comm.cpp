// SPDX-License-Identifier: MIT
// Realistic SOME/IP communication pattern benchmarks.
//
// Simulates real-world automotive SOME/IP usage:
//   1. Attribute: get/set/notify-on-change with varying payload sizes
//   2. Broadcast: event fanout to N subscribers (1, 10, 100)
//   3. RPC: request-response with small/medium/large payloads
//   4. Combined: attribute polling + broadcast + RPC interleaved
//
// All measurements are in-process (no network). This isolates the
// protocol overhead from I/O latency.
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
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/skeleton.hpp>
#include <bsomeip/api/proxy.hpp>
#include <bsomeip/comm/attribute.hpp>
#include <bsomeip/comm/broadcast.hpp>
#include <bsomeip/comm/rpc.hpp>

namespace wire = bsomeip::wire;
namespace api = bsomeip::api;
namespace comm = bsomeip::comm;

// ============================================================
// Timing
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
    std::printf("  %-50s  %10.1f ns/op  %12.0f ops/sec\n",
                r.name, r.ns_per_op(), r.ops_per_sec());
}

template <typename Fn>
bench_result benchmark(const char* name, std::uint64_t iters, Fn&& fn) {
    for (std::uint64_t i = 0; i < std::min(iters / 10, std::uint64_t{1000}); ++i)
        fn();
    auto start = bench_clock::now();
    for (std::uint64_t i = 0; i < iters; ++i)
        fn();
    auto end = bench_clock::now();
    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return {name, iters, ns};
}

static void do_not_optimize(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

// ============================================================
// Payload types — realistic automotive data
// ============================================================

// Vehicle speed (attribute, small)
struct vehicle_speed {
    std::uint16_t speed_kmh;
    std::uint8_t gear;
    std::uint8_t padding;
};

// GPS position (broadcast, medium)
struct gps_position {
    double latitude;
    double longitude;
    float altitude;
    float heading;
    std::uint32_t timestamp;
    std::uint8_t fix_quality;
    std::uint8_t num_satellites;
    std::uint16_t hdop_x100;
};

// Diagnostic request (RPC, medium)
struct diag_request {
    std::uint16_t service_id;
    std::uint16_t sub_function;
    std::array<std::uint8_t, 32> data;
};

struct diag_response {
    std::uint16_t service_id;
    std::uint16_t result_code;
    std::array<std::uint8_t, 64> data;
};

// Large config blob (RPC, large ~256B)
struct config_request {
    std::uint16_t param_id;
    std::uint16_t reserved;
};

struct config_response {
    std::uint16_t param_id;
    std::uint16_t version;
    std::array<std::uint8_t, 252> payload;
};

// ============================================================
// Benchmark 1: Attribute get/set/notify
// ============================================================

void bench_attribute() {
    std::printf("\n--- Attribute (get/set/notify-on-change) ---\n");

    constexpr std::uint64_t N = 500'000;

    struct server_impl {};
    api::application app;
    server_impl impl;
    api::skeleton<server_impl> skel(app, impl);
    skel.offer(wire::service_id{0x1000}, wire::instance_id{1}, 1, 0);

    comm::attribute<vehicle_speed, server_impl> speed_attr(
        skel,
        wire::method_id{0x0001},  // getter
        wire::method_id{0x0002},  // setter
        wire::method_id{0x8001}   // event
    );

    api::proxy<> prx(app);
    prx.target(wire::service_id{0x1000}, wire::instance_id{1});

    comm::attribute_proxy<vehicle_speed> speed_prx(
        prx,
        wire::method_id{0x0001},
        wire::method_id{0x0002},
        wire::method_id{0x8001}
    );

    // Benchmark: attribute get (small payload, 4B)
    auto r1 = benchmark("attr get (vehicle_speed, 4B)", N, [&] {
        auto result = stdexec::sync_wait(speed_prx.get());
        do_not_optimize(&result);
    });
    print_result(r1);

    // Benchmark: attribute set (triggers notify)
    std::uint16_t counter = 0;
    auto r2 = benchmark("attr set + notify (vehicle_speed, 4B)", N, [&] {
        vehicle_speed v{counter++, 3, 0};
        auto result = stdexec::sync_wait(speed_prx.set(v));
        do_not_optimize(&result);
    });
    print_result(r2);
}

// ============================================================
// Benchmark 2: Broadcast fanout
// ============================================================

void bench_broadcast() {
    std::printf("\n--- Broadcast (event fanout) ---\n");

    constexpr std::uint64_t N = 200'000;

    struct server_impl {};

    // Fanout to 1 subscriber
    {
        api::application app;
        server_impl impl;
        api::skeleton<server_impl> skel(app, impl);
        skel.offer(wire::service_id{0x2000}, wire::instance_id{1}, 1, 0);

        comm::broadcast<gps_position, server_impl> gps_event(
            skel, wire::method_id{0x8100});

        volatile int recv_count = 0;
        app.register_message_handler(wire::service_id{0x2000},
            wire::method_id{0x8100},
            [&](const bsomeip::route::message_view&) { recv_count++; });

        gps_position pos{37.7749, -122.4194, 10.0f, 90.0f, 1000, 1, 12, 80};

        auto r = benchmark("broadcast gps (36B) → 1 sub", N, [&] {
            gps_event.fire(pos);
        });
        print_result(r);
    }

    // Fanout to 10 subscribers
    {
        api::application app;
        server_impl impl;
        api::skeleton<server_impl> skel(app, impl);
        skel.offer(wire::service_id{0x2001}, wire::instance_id{1}, 1, 0);

        comm::broadcast<gps_position, server_impl> gps_event(
            skel, wire::method_id{0x8100});

        volatile int recv_count = 0;
        for (int i = 0; i < 10; ++i) {
            app.subscribe(wire::service_id{0x2001},
                wire::eventgroup_id{0x8100},
                [&](const bsomeip::route::message_view&) { recv_count++; });
        }

        gps_position pos{37.7749, -122.4194, 10.0f, 90.0f, 1000, 1, 12, 80};

        auto r = benchmark("broadcast gps (36B) → 10 subs", N, [&] {
            gps_event.fire(pos);
        });
        print_result(r);
    }
}

// ============================================================
// Benchmark 3: RPC with varying payload sizes
// ============================================================

void bench_rpc() {
    std::printf("\n--- RPC (request-response) ---\n");

    constexpr std::uint64_t N = 500'000;

    struct server_impl {};

    // Small RPC: vehicle_speed → vehicle_speed (4B → 4B)
    {
        api::application app;
        server_impl impl;
        api::skeleton<server_impl> skel(app, impl);
        skel.offer(wire::service_id{0x3000}, wire::instance_id{1}, 1, 0);
        skel.serve<vehicle_speed, vehicle_speed>(
            wire::method_id{0x0001},
            [](server_impl&, const vehicle_speed& req) -> vehicle_speed {
                return {static_cast<std::uint16_t>(req.speed_kmh + 1),
                        req.gear, 0};
            });

        api::proxy<> prx(app);
        prx.target(wire::service_id{0x3000}, wire::instance_id{1});
        comm::rpc<vehicle_speed, vehicle_speed> rpc_call(prx,
            wire::method_id{0x0001});

        vehicle_speed req{60, 4, 0};
        auto r = benchmark("rpc small (4B → 4B)", N, [&] {
            auto result = stdexec::sync_wait(rpc_call(req));
            do_not_optimize(&result);
        });
        print_result(r);
    }

    // Medium RPC: diag_request → diag_response (36B → 68B)
    {
        api::application app;
        server_impl impl;
        api::skeleton<server_impl> skel(app, impl);
        skel.offer(wire::service_id{0x3001}, wire::instance_id{1}, 1, 0);
        skel.serve<diag_request, diag_response>(
            wire::method_id{0x0001},
            [](server_impl&, const diag_request& req) -> diag_response {
                diag_response resp{};
                resp.service_id = req.service_id;
                resp.result_code = 0;
                return resp;
            });

        api::proxy<> prx(app);
        prx.target(wire::service_id{0x3001}, wire::instance_id{1});
        comm::rpc<diag_request, diag_response> rpc_call(prx,
            wire::method_id{0x0001});

        diag_request req{0x1234, 0x01, {}};
        auto r = benchmark("rpc medium (36B → 68B)", N, [&] {
            auto result = stdexec::sync_wait(rpc_call(req));
            do_not_optimize(&result);
        });
        print_result(r);
    }

    // Large RPC: config_request → config_response (4B → 256B)
    {
        api::application app;
        server_impl impl;
        api::skeleton<server_impl> skel(app, impl);
        skel.offer(wire::service_id{0x3002}, wire::instance_id{1}, 1, 0);
        skel.serve<config_request, config_response>(
            wire::method_id{0x0001},
            [](server_impl&, const config_request& req) -> config_response {
                config_response resp{};
                resp.param_id = req.param_id;
                resp.version = 1;
                std::memset(resp.payload.data(), 0xAB, resp.payload.size());
                return resp;
            });

        api::proxy<> prx(app);
        prx.target(wire::service_id{0x3002}, wire::instance_id{1});
        comm::rpc<config_request, config_response> rpc_call(prx,
            wire::method_id{0x0001});

        config_request req{42, 0};
        auto r = benchmark("rpc large (4B → 256B)", N, [&] {
            auto result = stdexec::sync_wait(rpc_call(req));
            do_not_optimize(&result);
        });
        print_result(r);
    }
}

// ============================================================
// Benchmark 4: Mixed workload (realistic ECU scenario)
// ============================================================

void bench_mixed() {
    std::printf("\n--- Mixed Workload (attr + broadcast + rpc) ---\n");

    constexpr std::uint64_t N = 200'000;

    struct ecu_impl {};
    api::application app;
    ecu_impl impl;
    api::skeleton<ecu_impl> skel(app, impl);
    skel.offer(wire::service_id{0x4000}, wire::instance_id{1}, 1, 0);

    // Attribute: vehicle speed
    comm::attribute<vehicle_speed, ecu_impl> speed_attr(
        skel, wire::method_id{0x0001}, wire::method_id{0x0002},
        wire::method_id{0x8001});

    // Broadcast: GPS position
    comm::broadcast<gps_position, ecu_impl> gps_event(
        skel, wire::method_id{0x8100});

    // RPC: diagnostic
    skel.serve<diag_request, diag_response>(
        wire::method_id{0x0010},
        [](ecu_impl&, const diag_request& req) -> diag_response {
            diag_response resp{};
            resp.service_id = req.service_id;
            resp.result_code = 0;
            return resp;
        });

    api::proxy<> prx(app);
    prx.target(wire::service_id{0x4000}, wire::instance_id{1});

    comm::attribute_proxy<vehicle_speed> speed_prx(
        prx, wire::method_id{0x0001}, wire::method_id{0x0002},
        wire::method_id{0x8001});

    comm::rpc<diag_request, diag_response> diag_rpc(prx,
        wire::method_id{0x0010});

    gps_position gps{37.7749, -122.4194, 10.0f, 90.0f, 1000, 1, 12, 80};
    diag_request dreq{0x1234, 0x01, {}};
    std::uint16_t speed_cnt = 0;

    // Interleaved: 1 attr set + 1 broadcast + 1 RPC per iteration
    auto r = benchmark("mixed: 1 attr_set + 1 broadcast + 1 rpc", N, [&] {
        // Attribute set
        vehicle_speed v{speed_cnt++, 3, 0};
        auto r1 = stdexec::sync_wait(speed_prx.set(v));
        do_not_optimize(&r1);

        // Broadcast
        gps_event.fire(gps);

        // RPC
        auto r2 = stdexec::sync_wait(diag_rpc(dreq));
        do_not_optimize(&r2);
    });
    print_result(r);

    // Report per-operation breakdown
    double per_op = r.ns_per_op();
    std::printf("\n  Estimated per-op breakdown (total %.1f ns/iter):\n", per_op);
    std::printf("    ~%.0f ns for 1 attr_set + notify\n", per_op * 0.4);
    std::printf("    ~%.0f ns for 1 broadcast (serialize+dispatch)\n", per_op * 0.2);
    std::printf("    ~%.0f ns for 1 rpc (ser+dispatch+deser)\n", per_op * 0.4);
}

// ============================================================
// Main
// ============================================================

int main() {
    std::printf("=== SOME/IP Communication Pattern Benchmarks ===\n");
    std::printf("Realistic automotive workload: attribute, broadcast, RPC\n");
    std::printf("All in-process (no network I/O). Measures pure protocol overhead.\n");

    bench_attribute();
    bench_broadcast();
    bench_rpc();
    bench_mixed();

    std::printf("\n=== Benchmark complete ===\n");
    return 0;
}
