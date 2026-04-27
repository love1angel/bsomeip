// SPDX-License-Identifier: MIT
// SOME/IP Attribute: typed getter/setter with notification-on-change.
//
// An attribute represents a piece of state on the server side.
// Clients can get/set the value. When the value changes, all
// subscribers receive a notification event automatically.
//
// Server side:
//   comm::attribute<int32_t> speed(skel, method_get, method_set, event_id);
//   speed.set(42);  // notifies all subscribers
//
// Client side (via proxy):
//   auto val = sync_wait(speed_proxy.get());
//   sync_wait(speed_proxy.set(100));
//   speed_proxy.on_change([](int32_t v) { ... });
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/skeleton.hpp>
#include <bsomeip/api/proxy.hpp>

namespace bsomeip::comm {

// ============================================================
// Server-side attribute: holds a value, auto-notifies on change.
// ============================================================

template <typename T, typename Impl = void>
    requires std::is_aggregate_v<T>
class attribute {
public:
    attribute(api::skeleton<Impl>& skel,
              wire::method_id getter,
              wire::method_id setter,
              wire::method_id event)
        : skel_{skel}, event_{event}, value_{}
    {
        // Register getter handler: returns current value.
        skel_.template serve<empty_request, T>(getter,
            [this](Impl&, const empty_request&) -> T {
                return value_;
            });

        // Register setter handler: updates value, notifies if changed.
        skel_.template serve<T, set_response>(setter,
            [this](Impl&, const T& new_val) -> set_response {
                set(new_val);
                return {};
            });
    }

    // Get current value.
    const T& get() const noexcept { return value_; }

    // Set value and notify subscribers if changed.
    void set(const T& new_val) {
        // Simple byte comparison for aggregate types.
        bool changed = std::memcmp(&value_, &new_val, sizeof(T)) != 0;
        value_ = new_val;
        if (changed) {
            notify();
        }
    }

    // Force a notification (even if value hasn't changed).
    void notify() {
        skel_.notify(event_, value_);
    }

private:
    // Minimal request/response for getter/setter wire format.
    // Must have at least one member for reflection-based codec.
    struct empty_request { std::uint8_t _pad{}; };
    struct set_response  { std::uint8_t _pad{}; };

    api::skeleton<Impl>& skel_;
    wire::method_id event_;
    T value_;
};

// ============================================================
// Client-side attribute proxy: get/set/subscribe.
// ============================================================

template <typename T>
    requires std::is_aggregate_v<T>
class attribute_proxy {
public:
    // Minimal request/response for getter/setter wire format.
    struct empty_request { std::uint8_t _pad{}; };
    struct set_response  { std::uint8_t _pad{}; };

    attribute_proxy(api::proxy<>& prx,
                    wire::method_id getter,
                    wire::method_id setter,
                    wire::method_id event)
        : prx_{prx}, getter_{getter}, setter_{setter}, event_{event} {}

    // Async get: returns a sender that completes with T.
    auto get() {
        return prx_.template async_call<empty_request, T>(getter_, empty_request{});
    }

    // Async set: returns a sender that completes when ack received.
    auto set(const T& value) {
        return prx_.template async_call<T, set_response>(setter_, value);
    }

    // Subscribe to change notifications.
    void on_change(std::function<void(const T&)> handler) {
        prx_.subscribe(wire::eventgroup_id{event_.value},
            [handler = std::move(handler)](const route::message_view& view) {
                auto result = wire::deserialize<T>(view.payload);
                if (result) handler(*result);
            });
    }

private:
    api::proxy<>& prx_;
    wire::method_id getter_;
    wire::method_id setter_;
    wire::method_id event_;
};

} // namespace bsomeip::comm
