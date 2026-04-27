// SPDX-License-Identifier: MIT
// SOME/IP Broadcast: typed event fanout to N subscribers.
//
// Server side:
//   comm::broadcast<gps_data> gps_event(skel, event_id);
//   gps_event.fire(data);  // serializes once, dispatches to all subscribers
//
// Client side:
//   comm::broadcast_proxy<gps_data> gps(prx, event_id);
//   gps.subscribe([](const gps_data& d) { ... });
#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/skeleton.hpp>
#include <bsomeip/api/proxy.hpp>

namespace bsomeip::comm {

// ============================================================
// Server-side broadcast: fire-and-forget event to all subscribers.
// ============================================================

template <typename T, typename Impl = void>
    requires std::is_aggregate_v<T>
class broadcast {
public:
    broadcast(api::skeleton<Impl>& skel, wire::method_id event)
        : skel_{skel}, event_{event} {}

    // Fire the event: serializes T and notifies all subscribers.
    void fire(const T& payload) {
        skel_.notify(event_, payload);
    }

private:
    api::skeleton<Impl>& skel_;
    wire::method_id event_;
};

// ============================================================
// Client-side broadcast subscriber.
// ============================================================

template <typename T>
    requires std::is_aggregate_v<T>
class broadcast_proxy {
public:
    broadcast_proxy(api::proxy<>& prx, wire::method_id event)
        : prx_{prx}, event_{event} {}

    // Subscribe to broadcast events.
    void subscribe(std::function<void(const T&)> handler) {
        prx_.subscribe(wire::eventgroup_id{event_.value},
            [handler = std::move(handler)](const route::message_view& view) {
                auto result = wire::deserialize<T>(view.payload);
                if (result) handler(*result);
            });
    }

private:
    api::proxy<>& prx_;
    wire::method_id event_;
};

} // namespace bsomeip::comm
