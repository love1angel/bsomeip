// SPDX-License-Identifier: MIT
// Phase 4 tests: application, skeleton, proxy with sender-based API.
// Key: async_call returns a sender, composed with sync_wait / then / let_value.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <tuple>
#include <vector>

#include <stdexec/execution.hpp>

#include <bsomeip/api/application.hpp>
#include <bsomeip/api/skeleton.hpp>
#include <bsomeip/api/proxy.hpp>
#include <bsomeip/config/config.hpp>
#include <bsomeip/wire/codec.hpp>

namespace wire = bsomeip::wire;
namespace api = bsomeip::api;
namespace config = bsomeip::config;

// Test service: add two ints
struct add_request { std::int32_t a; std::int32_t b; };
struct add_response { std::int32_t result; };

// Test service: temperature notification
struct temperature_event { float celsius; };

// Multiply service
struct mul_request { std::int32_t x; std::int32_t y; };
struct mul_response { std::int32_t product; };

// ============================================================
// Test 1: Application — offer + availability
// ============================================================
void test_app_offer_availability() {
    api::application app;

    bool avail_called = false;
    bool avail_value = false;

    app.register_availability_handler(
        wire::service_id{0x1234},
        [&](wire::service_id, wire::instance_id, bool available) {
            avail_called = true;
            avail_value = available;
        });

    app.offer_service(wire::service_id{0x1234}, wire::instance_id{0x0001}, 1, 0);
    assert(avail_called);
    assert(avail_value == true);

    avail_called = false;
    app.stop_offer_service(wire::service_id{0x1234}, wire::instance_id{0x0001});
    assert(avail_called);
    assert(avail_value == false);

    std::printf("[PASS] test_app_offer_availability\n");
}

// ============================================================
// Test 2: Application — session management
// ============================================================
void test_app_session() {
    api::application app;

    auto s1 = app.next_session();
    auto s2 = app.next_session();
    assert(s1 != s2);
    assert(s1 == wire::session_id{1});
    assert(s2 == wire::session_id{2});

    std::printf("[PASS] test_app_session\n");
}

// ============================================================
// Test 3: Message — create request/response/notification
// ============================================================
void test_message_create() {
    auto req = api::message::create_request(
        wire::service_id{0x1234}, wire::method_id{0x0001},
        wire::client_id{0x0010}, wire::session_id{1}, 8);
    assert(req.data.size() == wire::header_size + 8);
    assert(req.header().service() == wire::service_id{0x1234});
    assert(req.header().msg_type() == wire::message_type::request);

    auto resp = api::message::create_response(req.header(), 4);
    assert(resp.header().msg_type() == wire::message_type::response);
    assert(resp.header().session() == wire::session_id{1});

    auto notif = api::message::create_notification(
        wire::service_id{0x1234}, wire::method_id{0x8001}, 4);
    assert(notif.header().msg_type() == wire::message_type::notification);

    std::printf("[PASS] test_message_create\n");
}

// ============================================================
// Test 4: Skeleton serve() + Proxy async_call() with sync_wait
// THE key sender-based test.
// ============================================================
void test_async_call_sync_wait() {
    api::application app;

    // Server side
    struct calc_impl {
        add_response add(const add_request& req) {
            return {req.a + req.b};
        }
    };

    calc_impl impl;
    api::skeleton<calc_impl> skel(app, impl);
    skel.offer(wire::service_id{0x1234}, wire::instance_id{0x0001}, 1, 0);
    skel.serve<add_request, add_response>(
        wire::method_id{0x0001},
        [](calc_impl& svc, const add_request& req) -> add_response {
            return svc.add(req);
        });

    // Client side — sender-based
    api::proxy<> prx(app);
    prx.target(wire::service_id{0x1234}, wire::instance_id{0x0001});

    // async_call returns a sender; sync_wait blocks until completion.
    // For in-process dispatch, the whole chain runs synchronously in start().
    auto result = stdexec::sync_wait(
        prx.async_call<add_request, add_response>(
            wire::method_id{0x0001}, add_request{10, 20})
    );

    assert(result.has_value());
    auto [resp] = *result;
    assert(resp.result == 30);

    std::printf("[PASS] test_async_call_sync_wait\n");
}

// ============================================================
// Test 5: async_call composed with then()
// ============================================================
void test_async_call_then() {
    api::application app;

    struct calc_impl {
        add_response add(const add_request& req) {
            return {req.a + req.b};
        }
    };

    calc_impl impl;
    api::skeleton<calc_impl> skel(app, impl);
    skel.offer(wire::service_id{0x1234}, wire::instance_id{0x0001});
    skel.serve<add_request, add_response>(
        wire::method_id{0x0001},
        [](calc_impl& svc, const add_request& req) { return svc.add(req); });

    api::proxy<> prx(app);
    prx.target(wire::service_id{0x1234}, wire::instance_id{0x0001});

    // Compose: async_call | then(transform) | sync_wait
    auto result = stdexec::sync_wait(
        prx.async_call<add_request, add_response>(
            wire::method_id{0x0001}, add_request{100, 200})
        | stdexec::then([](add_response resp) -> int {
            return resp.result * 2;
        })
    );

    assert(result.has_value());
    auto [doubled] = *result;
    assert(doubled == 600);

    std::printf("[PASS] test_async_call_then\n");
}

// ============================================================
// Test 6: Multiple async_call in sequence
// ============================================================
void test_async_call_sequence() {
    api::application app;

    struct math_impl {
        add_response add(const add_request& r) { return {r.a + r.b}; }
        mul_response mul(const mul_request& r) { return {r.x * r.y}; }
    };

    math_impl impl;
    api::skeleton<math_impl> skel(app, impl);
    skel.offer(wire::service_id{0x1234}, wire::instance_id{0x0001});
    skel.serve<add_request, add_response>(
        wire::method_id{0x0001},
        [](math_impl& s, const add_request& r) { return s.add(r); });
    skel.serve<mul_request, mul_response>(
        wire::method_id{0x0002},
        [](math_impl& s, const mul_request& r) { return s.mul(r); });

    api::proxy<> prx(app);
    prx.target(wire::service_id{0x1234}, wire::instance_id{0x0001});

    // Call add
    auto r1 = stdexec::sync_wait(
        prx.async_call<add_request, add_response>(
            wire::method_id{0x0001}, add_request{3, 4}));
    assert(r1.has_value());
    assert(std::get<0>(*r1).result == 7);

    // Call mul
    auto r2 = stdexec::sync_wait(
        prx.async_call<mul_request, mul_response>(
            wire::method_id{0x0002}, mul_request{5, 6}));
    assert(r2.has_value());
    assert(std::get<0>(*r2).product == 30);

    // Chain: add result → mul
    auto r3 = stdexec::sync_wait(
        prx.async_call<add_request, add_response>(
            wire::method_id{0x0001}, add_request{10, 20})
        | stdexec::then([](add_response resp) {
            return mul_request{resp.result, 3};
        })
        | stdexec::let_value([&prx](mul_request req) {
            return prx.async_call<mul_request, mul_response>(
                wire::method_id{0x0002}, req);
        })
    );
    assert(r3.has_value());
    assert(std::get<0>(*r3).product == 90); // (10+20) * 3

    std::printf("[PASS] test_async_call_sequence\n");
}

// ============================================================
// Test 7: Skeleton notify routes through app
// ============================================================
void test_skeleton_notify() {
    api::application app;

    struct temp_service {};
    temp_service impl;
    api::skeleton<temp_service> skel(app, impl);
    skel.offer(wire::service_id{0x5678}, wire::instance_id{0x0001});

    // Subscribe via dispatcher to receive notifications
    bool received = false;
    float recv_temp = 0;
    app.router().get_dispatcher().register_handler(
        wire::service_id{0x5678}, wire::method_id{0x8001},
        [&](const bsomeip::route::message_view& view) {
            received = true;
            auto parsed = wire::deserialize<temperature_event>(view.payload);
            if (parsed) recv_temp = parsed->celsius;
        });

    temperature_event evt{36.5f};
    skel.notify(wire::method_id{0x8001}, evt);

    assert(received);
    assert(recv_temp > 36.4f && recv_temp < 36.6f);

    std::printf("[PASS] test_skeleton_notify\n");
}

// ============================================================
// Test 8: Proxy build_request (low-level API still works)
// ============================================================
void test_proxy_build_request() {
    api::application app;

    api::proxy<> prx(app);
    prx.target(wire::service_id{0x1234}, wire::instance_id{0x0001});

    add_request req{5, 7};
    auto result = prx.build_request(wire::method_id{0x0001}, req);
    assert(result.has_value());
    assert(result->header().service() == wire::service_id{0x1234});
    assert(result->header().msg_type() == wire::message_type::request);

    auto parsed = wire::deserialize<add_request>(result->payload());
    assert(parsed.has_value());
    assert(parsed->a == 5 && parsed->b == 7);

    std::printf("[PASS] test_proxy_build_request\n");
}

// ============================================================
// Test 9: Pending response tracking in application
// ============================================================
void test_app_pending_response() {
    api::application app;

    struct ctx_t {
        bool callback_fired{false};
        wire::return_code recv_rc{};
    } ctx;

    app.register_pending(wire::session_id{42}, &ctx,
        [](void* p, std::span<const std::byte>, wire::return_code rc) {
            auto* c = static_cast<ctx_t*>(p);
            c->callback_fired = true;
            c->recv_rc = rc;
        });

    // Build a response message with session 42
    auto msg = api::message::create(4);
    auto hdr = msg.header();
    hdr.set_service(wire::service_id{0x1234});
    hdr.set_method(wire::method_id{0x0001});
    hdr.set_session(wire::session_id{42});
    hdr.set_msg_type(wire::message_type::response);
    hdr.set_ret_code(wire::return_code::e_ok);

    bool routed = app.route(std::span<const std::byte>{msg.data});
    assert(routed);
    assert(ctx.callback_fired);
    assert(ctx.recv_rc == wire::return_code::e_ok);

    // Second route with same session should NOT match (one-shot)
    ctx.callback_fired = false;
    app.route(std::span<const std::byte>{msg.data});
    assert(!ctx.callback_fired);

    std::printf("[PASS] test_app_pending_response\n");
}

// ============================================================
// Test 10: Static config
// ============================================================
void test_static_config() {
    constexpr config::static_config cfg{
        .name = "test_app",
        .client = wire::client_id{0x0042},
        .unicast = {10, 0, 0, 1, 30000},
        .sd_multicast = {239, 244, 224, 245, 30490},
        .sd_cyclic_offer_delay_ms = 2000,
    };

    static_assert(cfg.name == "test_app");
    static_assert(cfg.client == wire::client_id{0x0042});
    static_assert(cfg.unicast.address[0] == 10);
    static_assert(cfg.sd_cyclic_offer_delay_ms == 2000);

    auto timing = config::get_sd_timing(cfg);
    assert(timing.cyclic_offer_delay_ms == 2000);

    std::printf("[PASS] test_static_config\n");
}

int main() {
    test_app_offer_availability();
    test_app_session();
    test_message_create();
    test_async_call_sync_wait();
    test_async_call_then();
    test_async_call_sequence();
    test_skeleton_notify();
    test_proxy_build_request();
    test_app_pending_response();
    test_static_config();

    std::printf("\n=== All API tests passed ===\n");
    return 0;
}
