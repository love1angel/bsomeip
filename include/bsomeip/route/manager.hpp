// SPDX-License-Identifier: MIT
// Routing manager: orchestrates local dispatch and remote endpoint routing.
// Combines registry + dispatcher into a single entry point for message handling.
#pragma once

#include <cstddef>
#include <span>

#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/route/registry.hpp>
#include <bsomeip/route/dispatcher.hpp>

namespace bsomeip::route {

// Routing decision
enum class route_target {
    local,       // dispatch to in-process handler
    remote,      // forward to remote endpoint
    unknown,     // service not found
};

class routing_manager {
public:
    // Access the service registry.
    registry& get_registry() noexcept { return registry_; }
    const registry& get_registry() const noexcept { return registry_; }

    // Access the message dispatcher.
    dispatcher& get_dispatcher() noexcept { return dispatcher_; }
    const dispatcher& get_dispatcher() const noexcept { return dispatcher_; }

    // Determine where a message should be routed.
    route_target resolve(wire::service_id service,
                         wire::instance_id instance) const {
        auto info = registry_.find(service, instance);
        if (!info) return route_target::unknown;
        return info->is_local ? route_target::local : route_target::remote;
    }

    // Route an incoming message: dispatches locally or returns the target info.
    // Returns true if handled locally, false if needs remote forwarding or unknown.
    bool route_message(std::span<const std::byte> raw_message) {
        if (raw_message.size() < wire::header_size)
            return false;

        // Parse header (read-only)
        wire::header_view hdr{const_cast<std::byte*>(raw_message.data())};
        auto payload = raw_message.subspan(wire::header_size);
        if (payload.size() < hdr.payload_length())
            return false;
        payload = payload.first(hdr.payload_length());

        // SD messages are handled by the SD module, not normal dispatch
        if (hdr.service() == wire::sd_service &&
            hdr.method() == wire::sd_method) {
            return false;
        }

        // Notifications go to eventgroup subscribers (requires external mapping)
        if (hdr.msg_type() == wire::message_type::notification) {
            // Event dispatch needs eventgroup mapping — caller should use
            // dispatcher.notify_subscribers() directly with the known eventgroup.
            return dispatcher_.dispatch(hdr, payload);
        }

        // Request/response dispatch by (service, method)
        return dispatcher_.dispatch(hdr, payload);
    }

    // Offer a local service.
    void offer_service(wire::service_id service, wire::instance_id instance,
                       std::uint8_t major = 0, std::uint32_t minor = 0) {
        service_info info{
            .service = service,
            .instance = instance,
            .major_version = major,
            .minor_version = minor,
            .ttl = 0xFFFFFF,
            .is_local = true,
        };
        bool is_new = registry_.offer(info);
        if (is_new) {
            dispatcher_.notify_availability(service, instance, true);
        }
    }

    // Stop offering a local service.
    void stop_offer_service(wire::service_id service,
                            wire::instance_id instance) {
        if (registry_.stop_offer(service, instance)) {
            dispatcher_.notify_availability(service, instance, false);
        }
    }

    // Register a remote service (discovered via SD).
    void add_remote_service(const service_info& info) {
        bool is_new = registry_.offer(info);
        if (is_new) {
            dispatcher_.notify_availability(info.service, info.instance, true);
        }
    }

    // Remove a remote service.
    void remove_remote_service(wire::service_id service,
                               wire::instance_id instance) {
        if (registry_.stop_offer(service, instance)) {
            dispatcher_.notify_availability(service, instance, false);
        }
    }

private:
    registry registry_;
    dispatcher dispatcher_;
};

} // namespace bsomeip::route
