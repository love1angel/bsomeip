// SPDX-License-Identifier: MIT
// In-process service registry: maps (service_id, instance_id) to service info.
// Thread-safe via shared_mutex (readers can run concurrently).
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/sd/option.hpp>

namespace bsomeip::route {

// Endpoint address for a remote service.
struct endpoint_info {
    sd::l4_protocol protocol{sd::l4_protocol::udp};
    std::array<std::uint8_t, 4> address{};  // IPv4
    std::uint16_t port{};
};

// Information about an offered service.
struct service_info {
    wire::service_id  service{};
    wire::instance_id instance{};
    std::uint8_t      major_version{};
    std::uint32_t     minor_version{};
    std::uint32_t     ttl{};  // seconds, 0xFFFFFF = infinite

    std::optional<endpoint_info> tcp{};
    std::optional<endpoint_info> udp{};

    bool is_local{false};  // offered by this process
};

// Combined key for registry lookup.
struct service_key {
    wire::service_id  service;
    wire::instance_id instance;

    bool operator==(const service_key&) const = default;
};

struct service_key_hash {
    std::size_t operator()(const service_key& k) const noexcept {
        auto h1 = std::hash<std::uint16_t>{}(k.service.value);
        auto h2 = std::hash<std::uint16_t>{}(k.instance.value);
        return h1 ^ (h2 << 16);
    }
};

// Thread-safe service registry.
class registry {
public:
    // Register a service. Returns true if newly inserted, false if updated.
    bool offer(const service_info& info) {
        std::unique_lock lock{mu_};
        auto key = service_key{info.service, info.instance};
        auto [it, inserted] = services_.try_emplace(key, info);
        if (!inserted) it->second = info;
        return inserted;
    }

    // Remove a service offer. Returns true if it existed.
    bool stop_offer(wire::service_id service,
                    wire::instance_id instance) {
        std::unique_lock lock{mu_};
        return services_.erase({service, instance}) > 0;
    }

    // Find a specific service instance.
    std::optional<service_info> find(wire::service_id service,
                                     wire::instance_id instance) const {
        std::shared_lock lock{mu_};
        auto it = services_.find({service, instance});
        if (it != services_.end()) return it->second;
        return std::nullopt;
    }

    // Find all instances of a service.
    std::vector<service_info> find_all(wire::service_id service) const {
        std::shared_lock lock{mu_};
        std::vector<service_info> result;
        for (const auto& [key, info] : services_) {
            if (key.service == service) result.push_back(info);
        }
        return result;
    }

    // Iterate all services.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        std::shared_lock lock{mu_};
        for (const auto& [key, info] : services_) {
            fn(info);
        }
    }

    std::size_t size() const {
        std::shared_lock lock{mu_};
        return services_.size();
    }

    void clear() {
        std::unique_lock lock{mu_};
        services_.clear();
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<service_key, service_info, service_key_hash> services_;
};

} // namespace bsomeip::route
