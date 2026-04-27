// SPDX-License-Identifier: MIT
// Skeleton: server-side service dispatch with sender integration.
//
// Key API: serve<Req, Resp>(method, handler_fn) — registers a method handler.
//   Handler takes (Impl&, const Req&) → Resp.
//   Responses are routed back through the application, enabling
//   proxy::async_call senders to complete.
//
// TODO(gcc16-annotations): Once annotations_of() works at compile time,
// the skeleton can auto-discover service/method IDs from the struct.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/api/application.hpp>

namespace bsomeip::api {

template <typename Impl, typename Desc = void>
class skeleton {
public:
    skeleton(application& app, Impl& impl)
        : app_{app}, impl_{impl} {}

    // --- Primary API: serve<Req, Resp> ---
    // Registers a method handler that:
    //   1. Deserializes the request payload
    //   2. Calls handler(impl, request) → response
    //   3. Serializes the response
    //   4. Routes response back through the application
    //      (this completes any pending proxy::async_call senders)
    template <typename Req, typename Resp, typename Handler>
        requires std::invocable<Handler&, Impl&, const Req&>
    void serve(wire::method_id method_id,
               Handler handler) {
        app_.register_message_handler(service_, method_id,
            [this, handler = std::move(handler)]
            (const route::message_view& view) mutable {
                // Deserialize request
                auto req_result = wire::deserialize<Req>(view.payload);
                if (!req_result) return;

                // Call handler
                auto resp = handler(impl_, *req_result);

                // Serialize response
                auto msg = message::create_response(view.header, sizeof(Resp));
                auto ser_result = wire::serialize(msg.payload(), resp);
                if (!ser_result) return;

                msg.header().set_payload_length(
                    static_cast<wire::length_t>(*ser_result));
                msg.data.resize(wire::header_size + *ser_result);

                // Route response through application — this enables
                // session-based pending callbacks (proxy::async_call)
                // to fire, completing the sender chain.
                app_.route(std::span<const std::byte>{msg.data});
            });
    }

    // Fire-and-forget method (request_no_return).
    template <typename Req, typename Handler>
        requires std::invocable<Handler&, Impl&, const Req&>
    void serve_oneway(wire::method_id method_id,
                      Handler handler) {
        app_.register_message_handler(service_, method_id,
            [this, handler = std::move(handler)]
            (const route::message_view& view) mutable {
                auto req_result = wire::deserialize<Req>(view.payload);
                if (!req_result) return;
                handler(impl_, *req_result);
            });
    }

    // Offer the service (registers in routing + starts SD offer).
    void offer(wire::service_id service, wire::instance_id instance,
               std::uint8_t major = 0, std::uint32_t minor = 0) {
        service_ = service;
        instance_ = instance;
        app_.offer_service(service, instance, major, minor);
    }

    void stop() {
        app_.stop_offer_service(service_, instance_);
    }

    // Send a notification to subscribers.
    // Routes through application so eventgroup subscribers are notified.
    template <typename T>
        requires std::is_aggregate_v<T>
    void notify(wire::method_id event_id, const T& payload) {
        auto msg = message::create_notification(service_, event_id, 256);
        auto result = wire::serialize(msg.payload(), payload);
        if (!result) return;
        msg.header().set_payload_length(
            static_cast<wire::length_t>(*result));
        msg.data.resize(wire::header_size + *result);
        app_.route(std::span<const std::byte>{msg.data});
    }

    // --- Legacy API (backward compat) ---

    template <typename Req, typename Resp, typename Handler>
        requires std::invocable<Handler&, Impl&, const Req&>
    void register_method(wire::method_id method_id,
                         Handler handler) {
        serve<Req, Resp>(method_id, std::move(handler));
    }

    template <typename F>
    void set_send_callback(F&&) {
        // Deprecated: responses now route through application.
        // Kept for API compat but is a no-op.
    }

private:
    application& app_;
    Impl& impl_;
    wire::service_id service_{};
    wire::instance_id instance_{};
};

} // namespace bsomeip::api
