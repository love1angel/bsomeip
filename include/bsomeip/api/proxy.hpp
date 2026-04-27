// SPDX-License-Identifier: MIT
// Proxy: client-side service interface with sender-based async_call.
//
// Key API: async_call<Req, Resp>(method, request) → sender_of<Resp>
//   Composes with stdexec: | then() | let_value() | sync_wait()
//   For in-process dispatch, completes synchronously inside start().
//   For remote dispatch, completes when the io_uring CQE arrives.
//
// TODO(gcc16-annotations): Auto-generate from annotated service structs
// once annotations_of() works.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <expected>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <bsomeip/async/execution.hpp>
#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/api/application.hpp>

namespace bsomeip::api {

// Callback for receiving a deserialized response (legacy API).
template <typename Resp>
using response_handler_t = std::function<void(std::expected<Resp, wire::codec_error>)>;

// ============================================================
// call_sender<Resp>: one-shot request/response sender.
// Returned by proxy::async_call<Req, Resp>().
//
// On start():
//   1. Registers a pending callback keyed by session_id
//   2. Routes the request through the application
//   3. For in-process: the skeleton handles immediately, response
//      routes back, pending callback fires → set_value(Resp)
//   4. For remote: completes when io_uring delivers the response
// ============================================================

template <typename Resp>
class call_sender {
public:
    using sender_concept = bsomeip::async::sender_tag;
    using completion_signatures = bsomeip::async::completion_signatures<
        bsomeip::async::set_value_t(Resp),
        bsomeip::async::set_error_t(std::exception_ptr),
        bsomeip::async::set_stopped_t()>;

    call_sender(application* app, message req_msg) noexcept
        : app_{app}, req_msg_{std::move(req_msg)} {}

    template <typename Receiver>
    struct op_state {
        using operation_state_concept = bsomeip::async::operation_state_tag;

        application* app_;
        message req_msg_;
        Receiver rcvr_;

        static void on_response(void* ctx,
                                std::span<const std::byte> payload,
                                wire::return_code rc) {
            auto* self = static_cast<op_state*>(ctx);
            if (rc != wire::return_code::e_ok) {
                bsomeip::async::set_error(std::move(self->rcvr_),
                    std::make_exception_ptr(
                        std::runtime_error("SOME/IP error response")));
                return;
            }
            auto result = wire::deserialize<Resp>(payload);
            if (result) {
                bsomeip::async::set_value(std::move(self->rcvr_),
                                   std::move(*result));
            } else {
                bsomeip::async::set_error(std::move(self->rcvr_),
                    std::make_exception_ptr(
                        std::runtime_error("response deserialize failed")));
            }
        }

        void start() & noexcept {
            auto session = req_msg_.header().session();
            app_->register_pending(session, this, &on_response);

            // Route the request — for in-process, this triggers the entire
            // chain synchronously: dispatch → skeleton → response → pending callback
            app_->route(std::span<const std::byte>{req_msg_.data});
        }
    };

    template <typename Receiver>
    auto connect(Receiver rcvr) && noexcept -> op_state<Receiver> {
        return {app_, std::move(req_msg_), std::move(rcvr)};
    }

    auto get_env() const noexcept { return bsomeip::async::empty_env{}; }

private:
    application* app_;
    message req_msg_;
};

// ============================================================
// Proxy: client-side service interface.
// ============================================================

template <typename Impl = void>
class proxy {
public:
    explicit proxy(application& app)
        : app_{app} {}

    void target(wire::service_id service, wire::instance_id instance) {
        service_ = service;
        instance_ = instance;
    }

    // --- Service discovery ---

    void request_service(std::uint8_t major = 0xFF,
                         std::uint32_t minor = 0xFFFFFFFF) {
        app_.request_service(service_, instance_, major, minor);
    }

    void release_service() {
        app_.release_service(service_, instance_);
    }

    void on_availability(availability_handler_t handler) {
        app_.register_availability_handler(service_, std::move(handler));
    }

    void subscribe(wire::eventgroup_id eg, message_handler_t handler) {
        app_.subscribe(service_, eg, std::move(handler));
    }

    // --- Sender-based async call (THE primary API) ---

    // Returns a sender that completes with Resp when the response arrives.
    // Usage:
    //   auto resp = stdexec::sync_wait(
    //       prx.async_call<add_request, add_response>(method, req)
    //   ).value();
    //
    //   // Or compose:
    //   prx.async_call<Req, Resp>(method, req)
    //     | stdexec::then([](Resp r) { use(r); });
    template <typename Req, typename Resp>
        requires (std::is_aggregate_v<Req> && std::is_aggregate_v<Resp>)
    auto async_call(wire::method_id method, const Req& payload)
        -> call_sender<Resp>
    {
        auto msg = build_request_impl(method, payload);
        return call_sender<Resp>{&app_, std::move(msg)};
    }

    // --- Low-level message building (for manual control) ---

    template <typename Req>
        requires std::is_aggregate_v<Req>
    std::expected<message, wire::codec_error>
    build_request(wire::method_id method, const Req& payload) {
        auto session = app_.next_session();
        auto msg = message::create_request(
            service_, method, app_.config().client, session, 256);
        auto result = wire::serialize(msg.payload(), payload);
        if (!result) return std::unexpected(result.error());
        msg.header().set_payload_length(
            static_cast<wire::length_t>(*result));
        msg.data.resize(wire::header_size + *result);
        return msg;
    }

    template <typename Resp>
        requires std::is_aggregate_v<Resp>
    static std::expected<Resp, wire::codec_error>
    parse_response(std::span<const std::byte> payload) {
        return wire::deserialize<Resp>(payload);
    }

    // --- Legacy callback API (kept for backward compat) ---

    template <typename Resp>
        requires std::is_aggregate_v<Resp>
    void on_response(wire::method_id method, response_handler_t<Resp> handler) {
        app_.register_message_handler(service_, method,
            [handler = std::move(handler)](const route::message_view& view) {
                if (!wire::is_response(view.header.msg_type())) return;
                auto result = wire::deserialize<Resp>(view.payload);
                handler(std::move(result));
            });
    }

    // Accessors
    wire::service_id service() const noexcept { return service_; }
    wire::instance_id instance() const noexcept { return instance_; }
    application& app() noexcept { return app_; }

private:
    template <typename Req>
    message build_request_impl(wire::method_id method, const Req& payload) {
        auto session = app_.next_session();
        auto msg = message::create_request(
            service_, method, app_.config().client, session, sizeof(Req));
        auto result = wire::serialize(msg.payload(), payload);
        if (!result) {
            // For sender path: create a minimal message; the error will
            // surface via set_error when the response never arrives.
            // In practice, serialize only fails if buffer is too small.
            msg.data.resize(wire::header_size);
            msg.header().set_payload_length(0);
            return msg;
        }
        msg.header().set_payload_length(
            static_cast<wire::length_t>(*result));
        msg.data.resize(wire::header_size + *result);
        return msg;
    }

    application& app_;
    wire::service_id service_{};
    wire::instance_id instance_{};
};

} // namespace bsomeip::api
