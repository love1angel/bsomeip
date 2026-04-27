// SPDX-License-Identifier: MIT
// Phase 3 tests: SD entry/option/message codec, registry, dispatcher, routing manager, discovery.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>

#include <bsomeip/sd/entry.hpp>
#include <bsomeip/sd/option.hpp>
#include <bsomeip/sd/message.hpp>
#include <bsomeip/sd/discovery.hpp>
#include <bsomeip/route/registry.hpp>
#include <bsomeip/route/dispatcher.hpp>
#include <bsomeip/route/manager.hpp>

namespace wire = bsomeip::wire;
namespace sd = bsomeip::sd;
namespace route = bsomeip::route;

// ============================================================
// Test 1: SD entry round-trip (offer service)
// ============================================================
void test_sd_entry_offer() {
    auto entry = sd::make_offer_service(
        wire::service_id{0x1234}, wire::instance_id{0x0001},
        /*major=*/1, /*minor=*/0x00000002, /*ttl=*/3);

    std::array<std::byte, sd::entry_size> buf{};
    sd::write_entry(buf, entry);

    auto parsed = sd::read_entry(buf);
    assert(parsed.type == sd::entry_type::offer_service);
    assert(parsed.service == wire::service_id{0x1234});
    assert(parsed.instance == wire::instance_id{0x0001});
    assert(parsed.major_version == 1);
    assert(parsed.ttl == 3);
    assert(parsed.minor_version == 2);

    std::printf("[PASS] test_sd_entry_offer\n");
}

// ============================================================
// Test 2: SD entry round-trip (subscribe eventgroup)
// ============================================================
void test_sd_entry_subscribe() {
    auto entry = sd::make_subscribe(
        wire::service_id{0x5678}, wire::instance_id{0x0003},
        /*major=*/2, wire::eventgroup_id{0x0010}, /*ttl=*/0xFFFFFF);

    std::array<std::byte, sd::entry_size> buf{};
    sd::write_entry(buf, entry);

    auto parsed = sd::read_entry(buf);
    assert(parsed.type == sd::entry_type::subscribe_eventgroup);
    assert(parsed.service == wire::service_id{0x5678});
    assert(parsed.instance == wire::instance_id{0x0003});
    assert(parsed.major_version == 2);
    assert(parsed.ttl == 0xFFFFFF);
    assert(parsed.eventgroup == wire::eventgroup_id{0x0010});

    std::printf("[PASS] test_sd_entry_subscribe\n");
}

// ============================================================
// Test 3: IPv4 option round-trip
// ============================================================
void test_sd_ipv4_option() {
    auto opt = sd::make_ipv4_endpoint(192, 168, 1, 100,
                                       sd::l4_protocol::tcp, 30490);

    std::array<std::byte, sd::ipv4_option_size> buf{};
    sd::write_ipv4_option(buf, opt);

    auto parsed = sd::read_ipv4_option(buf);
    assert(parsed.type == sd::option_type::ipv4_endpoint);
    assert(parsed.address[0] == 192);
    assert(parsed.address[1] == 168);
    assert(parsed.address[2] == 1);
    assert(parsed.address[3] == 100);
    assert(parsed.protocol == sd::l4_protocol::tcp);
    assert(parsed.port == 30490);

    std::printf("[PASS] test_sd_ipv4_option\n");
}

// ============================================================
// Test 4: SD message round-trip (offer + endpoint option)
// ============================================================
void test_sd_message_roundtrip() {
    sd::sd_message msg;
    msg.flags = {.reboot = true, .unicast = true};

    auto offer = sd::make_offer_service(
        wire::service_id{0x1234}, wire::instance_id{0x0001},
        1, 0, 3);
    offer.index_1st = 0;
    offer.num_options_1st = 1;
    msg.entries.push_back(offer);

    msg.options.push_back(sd::make_ipv4_endpoint(
        10, 0, 0, 1, sd::l4_protocol::udp, 30501));

    // Serialize
    std::array<std::byte, 256> buf{};
    auto len = sd::serialize_sd(buf, msg);
    assert(len > 0);

    // Deserialize
    auto result = sd::deserialize_sd(std::span{buf.data(), len});
    assert(result.has_value());

    auto& parsed = result.value();
    assert(parsed.flags.reboot == true);
    assert(parsed.flags.unicast == true);
    assert(parsed.entries.size() == 1);
    assert(parsed.entries[0].type == sd::entry_type::offer_service);
    assert(parsed.entries[0].service == wire::service_id{0x1234});
    assert(parsed.entries[0].index_1st == 0);
    assert(parsed.entries[0].num_options_1st == 1);
    assert(parsed.options.size() == 1);

    auto* ipv4 = std::get_if<sd::ipv4_option>(&parsed.options[0]);
    assert(ipv4 != nullptr);
    assert(ipv4->address[0] == 10);
    assert(ipv4->port == 30501);

    std::printf("[PASS] test_sd_message_roundtrip\n");
}

// ============================================================
// Test 5: Full SD SOME/IP message with header
// ============================================================
void test_sd_full_message() {
    sd::sd_message msg;
    msg.flags = {.reboot = false, .unicast = true};
    msg.entries.push_back(sd::make_find_service(
        wire::service_id{0xFFFF}, wire::instance_id{0xFFFF},
        0xFF, 0xFFFFFFFF, 3));

    std::array<std::byte, 512> buf{};
    auto total = sd::build_sd_message(buf, msg, wire::session_id{42});
    assert(total > wire::header_size);

    // Verify SOME/IP header
    wire::header_view hdr{buf.data()};
    assert(hdr.service() == wire::sd_service);
    assert(hdr.method() == wire::sd_method);
    assert(hdr.session() == wire::session_id{42});
    assert(hdr.msg_type() == wire::message_type::notification);
    assert(hdr.ret_code() == wire::return_code::e_ok);
    assert(hdr.protocol_ver() == wire::protocol_version);

    // Verify payload deserializes
    auto payload = std::span{buf}.subspan(wire::header_size, hdr.payload_length());
    auto result = sd::deserialize_sd(payload);
    assert(result.has_value());
    assert(result->entries.size() == 1);
    assert(result->entries[0].type == sd::entry_type::find_service);

    std::printf("[PASS] test_sd_full_message\n");
}

// ============================================================
// Test 6: Service registry
// ============================================================
void test_registry() {
    route::registry reg;

    route::service_info info{
        .service = wire::service_id{0x1234},
        .instance = wire::instance_id{0x0001},
        .major_version = 1,
        .minor_version = 0,
        .ttl = 3,
        .is_local = true,
    };

    assert(reg.offer(info) == true);   // new
    assert(reg.offer(info) == false);  // update
    assert(reg.size() == 1);

    auto found = reg.find(wire::service_id{0x1234}, wire::instance_id{0x0001});
    assert(found.has_value());
    assert(found->is_local == true);
    assert(found->major_version == 1);

    // Not found
    auto missing = reg.find(wire::service_id{0x9999}, wire::instance_id{0x0001});
    assert(!missing.has_value());

    // Stop offer
    assert(reg.stop_offer(wire::service_id{0x1234}, wire::instance_id{0x0001}));
    assert(reg.size() == 0);
    assert(!reg.stop_offer(wire::service_id{0x1234}, wire::instance_id{0x0001}));

    std::printf("[PASS] test_registry\n");
}

// ============================================================
// Test 7: Dispatcher — register and dispatch
// ============================================================
void test_dispatcher() {
    route::dispatcher disp;

    bool handler_called = false;
    wire::service_id recv_service{};

    disp.register_handler(
        wire::service_id{0x1234}, wire::method_id{0x0001},
        [&](const route::message_view& view) {
            handler_called = true;
            recv_service = view.header.service();
        });

    // Build a minimal message header
    std::array<std::byte, wire::header_size> hdr_buf{};
    wire::header_view hdr{hdr_buf.data()};
    hdr.set_service(wire::service_id{0x1234});
    hdr.set_method(wire::method_id{0x0001});
    hdr.set_payload_length(0);
    hdr.set_msg_type(wire::message_type::request);

    bool dispatched = disp.dispatch(hdr, {});
    assert(dispatched);
    assert(handler_called);
    assert(recv_service == wire::service_id{0x1234});

    // Unknown method
    handler_called = false;
    hdr.set_method(wire::method_id{0x9999});
    dispatched = disp.dispatch(hdr, {});
    assert(!dispatched);
    assert(!handler_called);

    // Unregister
    disp.unregister_handler(wire::service_id{0x1234}, wire::method_id{0x0001});
    hdr.set_method(wire::method_id{0x0001});
    assert(!disp.dispatch(hdr, {}));

    std::printf("[PASS] test_dispatcher\n");
}

// ============================================================
// Test 8: Routing manager — offer + dispatch
// ============================================================
void test_routing_manager() {
    route::routing_manager rm;

    bool avail_called = false;
    bool avail_value = false;

    rm.get_dispatcher().on_availability(
        wire::service_id{0x1234},
        [&](wire::service_id, wire::instance_id, bool available) {
            avail_called = true;
            avail_value = available;
        });

    rm.offer_service(wire::service_id{0x1234}, wire::instance_id{0x0001}, 1, 0);
    assert(avail_called);
    assert(avail_value == true);

    auto target = rm.resolve(wire::service_id{0x1234}, wire::instance_id{0x0001});
    assert(target == route::route_target::local);

    auto unknown = rm.resolve(wire::service_id{0x9999}, wire::instance_id{0x0001});
    assert(unknown == route::route_target::unknown);

    // Stop offer
    avail_called = false;
    rm.stop_offer_service(wire::service_id{0x1234}, wire::instance_id{0x0001});
    assert(avail_called);
    assert(avail_value == false);

    std::printf("[PASS] test_routing_manager\n");
}

// ============================================================
// Test 9: Discovery state machine — offer lifecycle
// ============================================================
void test_discovery_offer() {
    using namespace std::chrono_literals;

    sd::sd_config config;
    config.initial_delay_min = 0ms;
    config.initial_delay_max = 0ms;
    config.repetition_base_delay = 100ms;
    config.repetition_max = 3;
    config.cyclic_offer_delay = 1000ms;
    config.offer_ttl = 3;

    sd::discovery disc{config};
    auto t0 = sd::clock_t::now();

    disc.start_offer(
        wire::service_id{0x1234}, wire::instance_id{0x0001},
        1, 0, {}, t0);

    // tick at t0: should produce the initial offer
    auto msgs = disc.tick(t0);
    assert(msgs.size() == 1);
    assert(msgs[0].entries.size() == 1);
    assert(msgs[0].entries[0].type == sd::entry_type::offer_service);
    assert(msgs[0].flags.reboot == true);

    // State should now be repetition
    assert(disc.offers()[0].phase == sd::sd_phase::repetition);

    // tick at t0 + 50ms: too early for 2nd offer
    msgs = disc.tick(t0 + 50ms);
    assert(msgs.empty());

    // tick at t0 + 100ms: 2nd offer (repetition count=1)
    msgs = disc.tick(t0 + 100ms);
    assert(msgs.size() == 1);

    // tick at t0 + 300ms: 3rd offer (repetition count=2, base*2=200ms after 2nd)
    msgs = disc.tick(t0 + 300ms);
    assert(msgs.size() == 1);

    // After repetition_max=3, should be in main phase
    assert(disc.offers()[0].phase == sd::sd_phase::main);

    // Stop offer
    disc.stop_offer(wire::service_id{0x1234}, wire::instance_id{0x0001});
    msgs = disc.tick(t0 + 400ms);
    assert(msgs.size() == 1);
    assert(msgs[0].entries[0].ttl == 0);  // stop offer

    std::printf("[PASS] test_discovery_offer\n");
}

// ============================================================
// Test 10: Discovery — find service
// ============================================================
void test_discovery_find() {
    using namespace std::chrono_literals;

    sd::sd_config config;
    config.initial_delay_min = 0ms;
    config.repetition_base_delay = 50ms;
    config.repetition_max = 2;
    config.find_ttl = 3;

    sd::discovery disc{config};
    auto t0 = sd::clock_t::now();

    disc.start_find(
        wire::service_id{0x5678}, wire::instance_id{0xFFFF},
        0xFF, 0xFFFFFFFF, t0);

    // Initial find
    auto msgs = disc.tick(t0);
    assert(msgs.size() == 1);
    assert(msgs[0].entries[0].type == sd::entry_type::find_service);

    // Repetition
    msgs = disc.tick(t0 + 50ms);
    assert(msgs.size() == 1);

    // After repetition_max, find goes to down (no main phase)
    assert(disc.finds()[0].phase == sd::sd_phase::down);

    // No more messages
    msgs = disc.tick(t0 + 200ms);
    assert(msgs.empty());

    std::printf("[PASS] test_discovery_find\n");
}

// ============================================================
// Test 11: SD message with multiple entries and options
// ============================================================
void test_sd_multi_entry() {
    sd::sd_message msg;
    msg.flags = {.reboot = true, .unicast = true};

    // Two offers with different options
    auto offer1 = sd::make_offer_service(
        wire::service_id{0x1111}, wire::instance_id{0x0001}, 1, 0, 3);
    offer1.index_1st = 0;
    offer1.num_options_1st = 1;

    auto offer2 = sd::make_offer_service(
        wire::service_id{0x2222}, wire::instance_id{0x0001}, 1, 0, 3);
    offer2.index_1st = 1;
    offer2.num_options_1st = 1;

    msg.entries.push_back(offer1);
    msg.entries.push_back(offer2);

    msg.options.push_back(sd::make_ipv4_endpoint(
        10, 0, 0, 1, sd::l4_protocol::tcp, 30501));
    msg.options.push_back(sd::make_ipv4_endpoint(
        10, 0, 0, 1, sd::l4_protocol::udp, 30502));

    std::array<std::byte, 512> buf{};
    auto len = sd::serialize_sd(buf, msg);
    assert(len > 0);

    auto result = sd::deserialize_sd(std::span{buf.data(), len});
    assert(result.has_value());
    assert(result->entries.size() == 2);
    assert(result->options.size() == 2);
    assert(result->entries[0].service == wire::service_id{0x1111});
    assert(result->entries[1].service == wire::service_id{0x2222});

    auto* opt0 = std::get_if<sd::ipv4_option>(&result->options[0]);
    auto* opt1 = std::get_if<sd::ipv4_option>(&result->options[1]);
    assert(opt0 && opt0->port == 30501);
    assert(opt1 && opt1->port == 30502);

    std::printf("[PASS] test_sd_multi_entry\n");
}

// ============================================================
// Test 12: Eventgroup subscription dispatch
// ============================================================
void test_eventgroup_dispatch() {
    route::dispatcher disp;

    int notify_count = 0;
    disp.subscribe(wire::service_id{0x1234}, wire::eventgroup_id{0x0001},
        [&](const route::message_view&) { ++notify_count; });
    disp.subscribe(wire::service_id{0x1234}, wire::eventgroup_id{0x0001},
        [&](const route::message_view&) { ++notify_count; });

    std::array<std::byte, wire::header_size> hdr_buf{};
    wire::header_view hdr{hdr_buf.data()};
    hdr.set_service(wire::service_id{0x1234});
    hdr.set_method(wire::method_id{0x8001});
    hdr.set_msg_type(wire::message_type::notification);
    hdr.set_payload_length(0);

    disp.notify_subscribers(wire::service_id{0x1234},
                            wire::eventgroup_id{0x0001},
                            hdr, {});
    assert(notify_count == 2);

    // Unsubscribe
    disp.unsubscribe(wire::service_id{0x1234}, wire::eventgroup_id{0x0001});
    disp.notify_subscribers(wire::service_id{0x1234},
                            wire::eventgroup_id{0x0001},
                            hdr, {});
    assert(notify_count == 2);  // no change

    std::printf("[PASS] test_eventgroup_dispatch\n");
}

int main() {
    test_sd_entry_offer();
    test_sd_entry_subscribe();
    test_sd_ipv4_option();
    test_sd_message_roundtrip();
    test_sd_full_message();
    test_registry();
    test_dispatcher();
    test_routing_manager();
    test_discovery_offer();
    test_discovery_find();
    test_sd_multi_entry();
    test_eventgroup_dispatch();

    std::printf("\n=== All route/SD tests passed ===\n");
    return 0;
}
