// SPDX-License-Identifier: MIT
// Constexpr configuration for bsomeip.
// Users define a config struct; all values resolved at compile time.
// Optional JSON loading for vsomeip-compatible dynamic config.
#pragma once

#include <cstdint>
#include <array>
#include <string_view>

#include <bsomeip/wire/types.hpp>

namespace bsomeip::config {

// Default SD multicast group and port (AUTOSAR standard)
inline constexpr std::string_view default_sd_multicast = "239.244.224.245";
inline constexpr std::uint16_t    default_sd_port      = 30490;

// Network endpoint descriptor (constexpr-friendly).
struct endpoint {
    std::array<std::uint8_t, 4> address{};
    std::uint16_t port{};

    constexpr endpoint() = default;
    constexpr endpoint(std::uint8_t a, std::uint8_t b,
                       std::uint8_t c, std::uint8_t d,
                       std::uint16_t p) noexcept
        : address{a, b, c, d}, port{p} {}
};

// Service binding: maps a service to its transport endpoints.
struct service_binding {
    wire::service_id  service{};
    wire::instance_id instance{};
    endpoint reliable{};    // TCP
    endpoint unreliable{};  // UDP
};

// Static (constexpr) application configuration.
// Users can specialize or create their own config struct.
struct static_config {
    std::string_view name{"bsomeip"};
    wire::client_id client{0x0001};

    // Unicast address
    endpoint unicast{192, 168, 1, 100, 0};

    // SD multicast
    endpoint sd_multicast{239, 244, 224, 245, 30490};

    // SD timing
    std::uint32_t sd_initial_delay_ms{100};
    std::uint32_t sd_repetition_base_delay_ms{10};
    std::uint32_t sd_repetition_max{3};
    std::uint32_t sd_cyclic_offer_delay_ms{1000};
    std::uint32_t sd_offer_ttl{3};

    // Service bindings (static array, set at compile time)
    // Users override this in their config.
    std::array<service_binding, 0> services{};
};

// Convert static_config SD timing to sd::sd_config
// (defined here to avoid circular include with sd/discovery.hpp)
struct sd_timing {
    std::uint32_t initial_delay_ms;
    std::uint32_t repetition_base_delay_ms;
    std::uint32_t repetition_max;
    std::uint32_t cyclic_offer_delay_ms;
    std::uint32_t offer_ttl;
};

constexpr sd_timing get_sd_timing(const static_config& cfg) noexcept {
    return {
        cfg.sd_initial_delay_ms,
        cfg.sd_repetition_base_delay_ms,
        cfg.sd_repetition_max,
        cfg.sd_cyclic_offer_delay_ms,
        cfg.sd_offer_ttl,
    };
}

} // namespace bsomeip::config
