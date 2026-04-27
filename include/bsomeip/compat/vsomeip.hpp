// SPDX-License-Identifier: MIT
// vsomeip compatibility shim.
//
// Provides a vsomeip-like API surface so existing vsomeip applications can
// migrate incrementally.  The types live in the `vsomeip` namespace and
// delegate to bsomeip internals.
//
// Migration path:
//   1. Replace #include <vsomeip/vsomeip.hpp> with
//      #include <bsomeip/compat/vsomeip.hpp>
//   2. Rebuild — most code compiles unchanged.
//   3. Gradually replace vsomeip::message with bsomeip typed APIs.
//   4. Remove this header when migration is complete.
//
// Limitations:
//   - Shared pointer semantics are emulated (vsomeip uses shared_ptr
//     everywhere); bsomeip uses value types and senders.
//   - No plugin system, no JSON config file parsing.
//   - Event subscription uses eventgroup_id, not raw event_id.
//   - Only the most commonly used subset is provided.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/api/application.hpp>
#include <bsomeip/route/dispatcher.hpp>

namespace vsomeip {

// ---- Type aliases matching vsomeip typedefs ----

using service_t    = std::uint16_t;
using instance_t   = std::uint16_t;
using method_t     = std::uint16_t;
using event_t      = std::uint16_t;
using eventgroup_t = std::uint16_t;
using client_t     = std::uint16_t;
using session_t    = std::uint16_t;
using major_version_t = std::uint8_t;
using minor_version_t = std::uint32_t;
using length_t     = std::uint32_t;
using ttl_t        = std::uint32_t;

// vsomeip constants
inline constexpr service_t    ANY_SERVICE    = 0xFFFF;
inline constexpr instance_t   ANY_INSTANCE   = 0xFFFF;
inline constexpr method_t     ANY_METHOD     = 0xFFFF;
inline constexpr event_t      ANY_EVENT      = 0xFFFF;
inline constexpr eventgroup_t ANY_EVENTGROUP = 0xFFFF;
inline constexpr major_version_t ANY_MAJOR   = 0xFF;
inline constexpr minor_version_t ANY_MINOR   = 0xFFFFFFFF;
inline constexpr major_version_t DEFAULT_MAJOR = 0x00;
inline constexpr minor_version_t DEFAULT_MINOR = 0x00000000;

// ---- message_type_e / return_code_e ----

enum class message_type_e : std::uint8_t {
    MT_REQUEST           = 0x00,
    MT_REQUEST_NO_RETURN = 0x01,
    MT_NOTIFICATION      = 0x02,
    MT_REQUEST_ACK       = 0x40,
    MT_RESPONSE          = 0x80,
    MT_ERROR             = 0x81,
    MT_RESPONSE_ACK      = 0xC0,
    MT_ERROR_ACK         = 0xC1,
    MT_UNKNOWN           = 0xFF,
};

enum class return_code_e : std::uint8_t {
    E_OK                  = 0x00,
    E_NOT_OK              = 0x01,
    E_UNKNOWN_SERVICE     = 0x02,
    E_UNKNOWN_METHOD      = 0x03,
    E_NOT_READY           = 0x04,
    E_NOT_REACHABLE       = 0x05,
    E_TIMEOUT             = 0x06,
    E_WRONG_PROTOCOL_VERSION = 0x07,
    E_WRONG_INTERFACE_VERSION = 0x08,
    E_MALFORMED_MESSAGE   = 0x09,
    E_WRONG_MESSAGE_TYPE  = 0x0A,
    E_UNKNOWN             = 0xFF,
};

// ---- state_type_e (application state) ----

enum class state_type_e : std::uint8_t {
    ST_REGISTERED   = 0x00,
    ST_DEREGISTERED = 0x01,
};

// ---- payload (wraps a byte buffer) ----

class payload {
public:
    payload() = default;

    void set_data(const std::uint8_t* data, length_t len) {
        data_.assign(reinterpret_cast<const std::byte*>(data),
                     reinterpret_cast<const std::byte*>(data) + len);
    }

    void set_data(const std::vector<std::uint8_t>& d) {
        data_.assign(reinterpret_cast<const std::byte*>(d.data()),
                     reinterpret_cast<const std::byte*>(d.data()) + d.size());
    }

    const std::uint8_t* get_data() const noexcept {
        return reinterpret_cast<const std::uint8_t*>(data_.data());
    }

    length_t get_length() const noexcept {
        return static_cast<length_t>(data_.size());
    }

    std::span<const std::byte> as_span() const noexcept {
        return data_;
    }

    std::vector<std::byte>& raw() noexcept { return data_; }

private:
    std::vector<std::byte> data_;
};

// ---- message (wraps bsomeip::api::message) ----

class message {
public:
    message() : inner_{bsomeip::api::message::create(0)} {}

    // --- Getters ---
    service_t get_service() const noexcept {
        return const_hdr().service().value;
    }
    instance_t get_instance() const noexcept {
        return instance_;
    }
    method_t get_method() const noexcept {
        return const_hdr().method().value;
    }
    client_t get_client() const noexcept {
        return const_hdr().client().value;
    }
    session_t get_session() const noexcept {
        return const_hdr().session().value;
    }
    message_type_e get_message_type() const noexcept {
        return static_cast<message_type_e>(
            static_cast<std::uint8_t>(const_hdr().msg_type()));
    }
    return_code_e get_return_code() const noexcept {
        return static_cast<return_code_e>(
            static_cast<std::uint8_t>(const_hdr().ret_code()));
    }
    major_version_t get_interface_version() const noexcept {
        return const_hdr().interface_ver();
    }

    // --- Setters ---
    void set_service(service_t s) {
        ensure_header();
        hdr().set_service(bsomeip::wire::service_id{s});
    }
    void set_instance(instance_t i) { instance_ = i; }
    void set_method(method_t m) {
        ensure_header();
        hdr().set_method(bsomeip::wire::method_id{m});
    }
    void set_client(client_t c) {
        ensure_header();
        hdr().set_client(bsomeip::wire::client_id{c});
    }
    void set_session(session_t s) {
        ensure_header();
        hdr().set_session(bsomeip::wire::session_id{s});
    }
    void set_message_type(message_type_e t) {
        ensure_header();
        hdr().set_msg_type(static_cast<bsomeip::wire::message_type>(
            static_cast<std::uint8_t>(t)));
    }
    void set_return_code(return_code_e rc) {
        ensure_header();
        hdr().set_ret_code(static_cast<bsomeip::wire::return_code>(
            static_cast<std::uint8_t>(rc)));
    }
    void set_interface_version(major_version_t v) {
        ensure_header();
        hdr().set_interface_ver(v);
    }

    // --- Payload ---
    std::shared_ptr<payload> get_payload() const {
        auto p = std::make_shared<payload>();
        auto span = inner_.payload();
        if (!span.empty()) {
            p->set_data(reinterpret_cast<const std::uint8_t*>(span.data()),
                        static_cast<length_t>(span.size()));
        }
        return p;
    }

    void set_payload(const std::shared_ptr<payload>& p) {
        if (!p) return;
        auto needed = bsomeip::wire::header_size + p->get_length();
        inner_.data.resize(needed, std::byte{0});
        auto dest = inner_.payload();
        std::memcpy(dest.data(), p->get_data(), p->get_length());
        hdr().set_payload_length(p->get_length());
    }

    // Access the underlying bsomeip message.
    bsomeip::api::message& native() noexcept { return inner_; }
    const bsomeip::api::message& native() const noexcept { return inner_; }

    // Raw bytes (for routing).
    std::span<const std::byte> raw() const noexcept { return inner_.data; }

private:
    void ensure_header() {
        if (inner_.data.size() < bsomeip::wire::header_size)
            inner_.data.resize(bsomeip::wire::header_size, std::byte{0});
    }

    bsomeip::wire::header_view hdr() noexcept {
        return bsomeip::wire::header_view{inner_.data.data()};
    }

    bsomeip::wire::const_header_view const_hdr() const noexcept {
        return bsomeip::wire::const_header_view{inner_.data.data()};
    }

    mutable bsomeip::api::message inner_;
    instance_t instance_{0x0001};
};

// ---- Factory functions (vsomeip style) ----

inline std::shared_ptr<payload> create_payload() {
    return std::make_shared<payload>();
}

inline std::shared_ptr<payload> create_payload(
        const std::uint8_t* data, std::uint32_t len) {
    auto p = std::make_shared<payload>();
    p->set_data(data, len);
    return p;
}

inline std::shared_ptr<message> create_message(bool is_request = true) {
    auto m = std::make_shared<message>();
    m->set_message_type(is_request ? message_type_e::MT_REQUEST
                                   : message_type_e::MT_RESPONSE);
    return m;
}

inline std::shared_ptr<message> create_request(bool reliable = true) {
    (void)reliable;
    return create_message(true);
}

inline std::shared_ptr<message> create_response(
        const std::shared_ptr<message>& req) {
    auto resp = std::make_shared<message>();
    resp->set_service(req->get_service());
    resp->set_instance(req->get_instance());
    resp->set_method(req->get_method());
    resp->set_client(req->get_client());
    resp->set_session(req->get_session());
    resp->set_interface_version(req->get_interface_version());
    resp->set_message_type(message_type_e::MT_RESPONSE);
    resp->set_return_code(return_code_e::E_OK);
    return resp;
}

inline std::shared_ptr<message> create_notification(bool reliable = false) {
    (void)reliable;
    auto m = std::make_shared<message>();
    m->set_message_type(message_type_e::MT_NOTIFICATION);
    return m;
}

// ---- Handler typedefs (vsomeip style) ----

using message_handler_t      = std::function<void(const std::shared_ptr<message>&)>;
using availability_handler_t = std::function<void(service_t, instance_t, bool)>;
using state_handler_t        = std::function<void(state_type_e)>;

// ---- application (wraps bsomeip::api::application) ----

class application {
public:
    explicit application(const std::string& name = "")
        : inner_{bsomeip::api::app_config{
              .name = name.empty() ? "bsomeip" : name}} {}

    // --- Lifecycle ---
    bool init() { return true; /* bsomeip has no late init */ }

    void start() {
        // In vsomeip, start() blocks and runs the event loop.
        // bsomeip uses io_uring senders; this is a no-op shim.
        // Real event loops should use bsomeip::io::uring_context directly.
        if (state_handler_)
            state_handler_(state_type_e::ST_REGISTERED);
    }

    void stop() {
        if (state_handler_)
            state_handler_(state_type_e::ST_DEREGISTERED);
    }

    const std::string& get_name() const noexcept {
        return inner_.config().name;
    }

    client_t get_client() const noexcept {
        return inner_.config().client.value;
    }

    // --- State handler ---
    void register_state_handler(state_handler_t handler) {
        state_handler_ = std::move(handler);
    }

    // --- Service offering ---
    void offer_service(service_t service, instance_t instance,
                       major_version_t major = DEFAULT_MAJOR,
                       minor_version_t minor = DEFAULT_MINOR) {
        inner_.offer_service(bsomeip::wire::service_id{service},
                             bsomeip::wire::instance_id{instance},
                             major, minor);
    }

    void stop_offer_service(service_t service, instance_t instance,
                            major_version_t = DEFAULT_MAJOR,
                            minor_version_t = DEFAULT_MINOR) {
        inner_.stop_offer_service(bsomeip::wire::service_id{service},
                                  bsomeip::wire::instance_id{instance});
    }

    // --- Service requesting ---
    void request_service(service_t service, instance_t instance,
                         major_version_t major = ANY_MAJOR,
                         minor_version_t minor = ANY_MINOR) {
        inner_.request_service(bsomeip::wire::service_id{service},
                               bsomeip::wire::instance_id{instance},
                               major, minor);
    }

    void release_service(service_t service, instance_t instance) {
        inner_.release_service(bsomeip::wire::service_id{service},
                               bsomeip::wire::instance_id{instance});
    }

    // --- Message handler ---
    void register_message_handler(service_t service, instance_t instance,
                                  method_t method,
                                  message_handler_t handler) {
        (void)instance;  // bsomeip dispatches by service+method only
        inner_.register_message_handler(
            bsomeip::wire::service_id{service},
            bsomeip::wire::method_id{method},
            [handler = std::move(handler)](
                const bsomeip::route::message_view& view) {
                // Wrap in vsomeip message
                auto msg = std::make_shared<vsomeip::message>();
                auto& raw = msg->native();
                auto total = bsomeip::wire::header_size + view.payload.size();
                raw.data.resize(total);
                std::memcpy(raw.data.data(), view.header.data(),
                            bsomeip::wire::header_size);
                if (!view.payload.empty()) {
                    std::memcpy(raw.data.data() + bsomeip::wire::header_size,
                                view.payload.data(), view.payload.size());
                }
                handler(msg);
            });
    }

    void unregister_message_handler(service_t service, instance_t /*instance*/,
                                    method_t method) {
        inner_.unregister_message_handler(bsomeip::wire::service_id{service},
                                          bsomeip::wire::method_id{method});
    }

    // --- Availability handler ---
    void register_availability_handler(service_t service, instance_t instance,
                                       availability_handler_t handler,
                                       major_version_t = ANY_MAJOR,
                                       minor_version_t = ANY_MINOR) {
        (void)instance;
        inner_.register_availability_handler(
            bsomeip::wire::service_id{service},
            [handler = std::move(handler)](
                bsomeip::wire::service_id s,
                bsomeip::wire::instance_id i, bool avail) {
                handler(s.value, i.value, avail);
            });
    }

    // --- Events / subscriptions ---
    void offer_event(service_t service, instance_t /*instance*/,
                     event_t event, const std::set<eventgroup_t>& /*groups*/,
                     bool /*is_field*/ = false) {
        (void)service; (void)event;
        // bsomeip events are registered implicitly via skeleton::notify.
    }

    void stop_offer_event(service_t, instance_t, event_t) {}

    void subscribe(service_t service, instance_t /*instance*/,
                   eventgroup_t eventgroup,
                   major_version_t = ANY_MAJOR,
                   event_t = ANY_EVENT) {
        // No-op in the compat layer; use subscribe_with_handler below,
        // or register a message_handler for the event method ID.
        (void)service; (void)eventgroup;
    }

    void unsubscribe(service_t service, instance_t /*instance*/,
                     eventgroup_t eventgroup) {
        inner_.unsubscribe(bsomeip::wire::service_id{service},
                           bsomeip::wire::eventgroup_id{eventgroup});
    }

    // --- Send ---
    void send(const std::shared_ptr<message>& msg) {
        if (!msg) return;
        inner_.route(msg->raw());
    }

    // --- Notify ---
    void notify(service_t service, instance_t /*instance*/,
                event_t event, const std::shared_ptr<payload>& pl) {
        auto notif = bsomeip::api::message::create_notification(
            bsomeip::wire::service_id{service},
            bsomeip::wire::method_id{event},
            pl ? pl->get_length() : 0);
        if (pl && pl->get_length() > 0) {
            std::memcpy(notif.payload().data(), pl->get_data(),
                        pl->get_length());
        }
        inner_.route(std::span<const std::byte>{notif.data});
    }

    // --- Access native bsomeip application ---
    bsomeip::api::application& native() noexcept { return inner_; }
    const bsomeip::api::application& native() const noexcept { return inner_; }

private:
    bsomeip::api::application inner_;
    state_handler_t state_handler_;
};

// ---- runtime (singleton factory, vsomeip style) ----

class runtime {
public:
    static std::shared_ptr<runtime> get() {
        static auto instance = std::make_shared<runtime>();
        return instance;
    }

    std::shared_ptr<application> create_application(
            const std::string& name = "") {
        return std::make_shared<application>(name);
    }

    std::shared_ptr<message> create_message(bool is_request = true) {
        return vsomeip::create_message(is_request);
    }

    std::shared_ptr<message> create_request(bool reliable = true) {
        return vsomeip::create_request(reliable);
    }

    std::shared_ptr<message> create_response(
            const std::shared_ptr<message>& req) {
        return vsomeip::create_response(req);
    }

    std::shared_ptr<payload> create_payload() {
        return vsomeip::create_payload();
    }

    std::shared_ptr<payload> create_payload(
            const std::uint8_t* data, std::uint32_t len) {
        return vsomeip::create_payload(data, len);
    }
};

} // namespace vsomeip
