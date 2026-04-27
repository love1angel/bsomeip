// SPDX-License-Identifier: MIT
// Application: top-level entry point for bsomeip.
// Owns the routing manager, discovery, and event loop.
// All async operations return stdexec senders.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/route/manager.hpp>
#include <bsomeip/sd/discovery.hpp>

namespace bsomeip::api {

// Re-export handler types for convenience
using message_handler_t     = route::message_handler_t;
using availability_handler_t = route::availability_handler_t;

// Application configuration
struct app_config {
    std::string name{"bsomeip"};
    wire::client_id client{0x0001};

    // SD configuration
    sd::sd_config sd{};
};

// Owning message buffer for responses and notifications.
struct message {
    std::vector<std::byte> data;

    wire::header_view header() noexcept {
        return wire::header_view{data.data()};
    }

    std::span<std::byte> payload() noexcept {
        if (data.size() <= wire::header_size) return {};
        return std::span{data}.subspan(wire::header_size);
    }

    std::span<const std::byte> payload() const noexcept {
        if (data.size() <= wire::header_size) return {};
        return std::span{data.data() + wire::header_size,
                         data.size() - wire::header_size};
    }

    // Allocate a message with header + payload space.
    static message create(std::size_t payload_size) {
        message m;
        m.data.resize(wire::header_size + payload_size, std::byte{0});
        auto hdr = m.header();
        hdr.set_protocol_ver(wire::protocol_version);
        hdr.set_payload_length(static_cast<wire::length_t>(payload_size));
        return m;
    }

    // Create a response from a request header.
    static message create_response(const wire::header_view& req,
                                   std::size_t payload_size = 0) {
        auto m = create(payload_size);
        auto hdr = m.header();
        hdr.set_service(req.service());
        hdr.set_method(req.method());
        hdr.set_client(req.client());
        hdr.set_session(req.session());
        hdr.set_interface_ver(req.interface_ver());
        hdr.set_msg_type(wire::message_type::response);
        hdr.set_ret_code(wire::return_code::e_ok);
        return m;
    }

    // Create a notification (event) message.
    static message create_notification(wire::service_id service,
                                       wire::method_id event,
                                       std::size_t payload_size = 0) {
        auto m = create(payload_size);
        auto hdr = m.header();
        hdr.set_service(service);
        hdr.set_method(event);
        hdr.set_msg_type(wire::message_type::notification);
        hdr.set_ret_code(wire::return_code::e_ok);
        return m;
    }

    // Create a request message.
    static message create_request(wire::service_id service,
                                  wire::method_id method,
                                  wire::client_id client,
                                  wire::session_id session,
                                  std::size_t payload_size = 0) {
        auto m = create(payload_size);
        auto hdr = m.header();
        hdr.set_service(service);
        hdr.set_method(method);
        hdr.set_client(client);
        hdr.set_session(session);
        hdr.set_msg_type(wire::message_type::request);
        hdr.set_ret_code(wire::return_code::e_ok);
        return m;
    }
};

// Application: the main bsomeip entry point.
// In production, owns the io event loop. For now, exposes the routing
// and SD building blocks so higher layers can compose senders.
class application {
public:
    explicit application(app_config config = {})
        : config_{std::move(config)}, discovery_{config_.sd} {
        pending_.resize(256);  // Pre-allocate for common session ID range
    }

    const app_config& config() const noexcept { return config_; }

    // --- Service offering (server side) ---

    void offer_service(wire::service_id service, wire::instance_id instance,
                       std::uint8_t major = 0, std::uint32_t minor = 0) {
        router_.offer_service(service, instance, major, minor);
        discovery_.start_offer(service, instance, major, minor);
    }

    void stop_offer_service(wire::service_id service,
                            wire::instance_id instance) {
        router_.stop_offer_service(service, instance);
        discovery_.stop_offer(service, instance);
    }

    // --- Service requesting (client side) ---

    void request_service(wire::service_id service, wire::instance_id instance,
                         std::uint8_t major = 0xFF, std::uint32_t minor = 0xFFFFFFFF) {
        discovery_.start_find(service, instance, major, minor);
    }

    void release_service(wire::service_id service, wire::instance_id instance) {
        discovery_.stop_find(service, instance);
    }

    // --- Handler registration ---

    void register_message_handler(wire::service_id service,
                                  wire::method_id method,
                                  message_handler_t handler) {
        router_.get_dispatcher().register_handler(service, method,
                                                   std::move(handler));
    }

    void unregister_message_handler(wire::service_id service,
                                    wire::method_id method) {
        router_.get_dispatcher().unregister_handler(service, method);
    }

    void register_availability_handler(wire::service_id service,
                                       availability_handler_t handler) {
        router_.get_dispatcher().on_availability(service, std::move(handler));
    }

    void subscribe(wire::service_id service, wire::eventgroup_id eg,
                   message_handler_t handler) {
        router_.get_dispatcher().subscribe(service, eg, std::move(handler));
    }

    void unsubscribe(wire::service_id service, wire::eventgroup_id eg) {
        router_.get_dispatcher().unsubscribe(service, eg);
    }

    // --- Message routing ---

    // Pending response callback: type-erased function pointer + context.
    // Zero allocation. The callback lifetime is managed by the caller (op_state).
    struct pending_entry {
        void* context{nullptr};
        void (*invoke)(void* ctx, std::span<const std::byte>, wire::return_code){nullptr};
    };

    // Register a one-shot pending response callback keyed by session_id.
    void register_pending(wire::session_id session, void* ctx,
                          void (*fn)(void*, std::span<const std::byte>, wire::return_code)) {
        // Session IDs are 16-bit; use a flat array for O(1) lookup.
        auto idx = session.value;
        if (idx >= pending_.size()) pending_.resize(idx + 1);
        pending_[idx] = {ctx, fn};
    }

    // Route a raw SOME/IP message through the application.
    // For responses: checks pending session-based callbacks first (enables async_call).
    // For requests/notifications: dispatches via the routing manager.
    bool route(std::span<const std::byte> raw_message) {
        if (raw_message.size() < wire::header_size)
            return false;

        wire::header_view hdr{const_cast<std::byte*>(raw_message.data())};

        // Responses: check pending callbacks by session_id first
        if (wire::is_response(hdr.msg_type())) {
            auto idx = hdr.session().value;
            if (idx < pending_.size() && pending_[idx].invoke) {
                auto entry = pending_[idx];
                pending_[idx] = {};  // one-shot: clear
                auto payload = raw_message.subspan(wire::header_size);
                auto plen = hdr.payload_length();
                if (payload.size() > plen) payload = payload.first(plen);
                entry.invoke(entry.context, payload, hdr.ret_code());
                return true;
            }
        }

        // Fall through to normal dispatch
        return router_.route_message(raw_message);
    }

    // --- Session management ---

    wire::session_id next_session() noexcept {
        auto s = session_counter_++;
        if (s == 0) s = session_counter_++;  // skip 0
        return wire::session_id{s};
    }

    // --- Accessors ---

    route::routing_manager& router() noexcept { return router_; }
    sd::discovery& discovery() noexcept { return discovery_; }

private:
    app_config config_;
    route::routing_manager router_;
    sd::discovery discovery_;
    std::uint16_t session_counter_{1};

    // Flat array indexed by session_id (16-bit). O(1) lookup, no hash, no allocation.
    std::vector<pending_entry> pending_;
};

} // namespace bsomeip::api
