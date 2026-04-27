// SPDX-License-Identifier: MIT
// SOME/IP Service Discovery state machine.
// Implements the SD protocol phases: INITIAL → REPETITION → MAIN.
// Timer-driven: call tick() with the current monotonic timestamp.
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>
#include <functional>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/sd/entry.hpp>
#include <bsomeip/sd/option.hpp>
#include <bsomeip/sd/message.hpp>

namespace bsomeip::sd {

// SD protocol phases per AUTOSAR SD spec
enum class sd_phase : std::uint8_t {
    down,              // not started
    initial_wait,      // waiting initial_delay before first offer
    repetition,        // exponential backoff offers
    main,              // cyclic offers at fixed interval
};

using clock_t = std::chrono::steady_clock;
using time_point = clock_t::time_point;
using duration = clock_t::duration;

// Configuration for SD timing (per AUTOSAR defaults)
struct sd_config {
    // Initial wait phase
    duration initial_delay_min{std::chrono::milliseconds{0}};
    duration initial_delay_max{std::chrono::milliseconds{3000}};

    // Repetition phase
    duration repetition_base_delay{std::chrono::milliseconds{10}};
    std::uint32_t repetition_max{3};

    // Main phase
    duration cyclic_offer_delay{std::chrono::milliseconds{1000}};

    // TTL for offered services (seconds)
    std::uint32_t offer_ttl{3};
    std::uint32_t find_ttl{3};
};

// Callback to send an SD message (the state machine produces messages,
// the transport layer sends them).
using sd_send_fn = std::function<void(const sd_message&)>;

// Per-service offer state tracked by the state machine.
struct offer_state {
    wire::service_id  service{};
    wire::instance_id instance{};
    std::uint8_t      major_version{};
    std::uint32_t     minor_version{};

    // Associated endpoint options
    std::vector<sd_option> options;

    // State machine
    sd_phase phase{sd_phase::down};
    time_point next_send{};
    std::uint32_t repetition_count{0};
};

// Per-service find state (client looking for a service).
struct find_state {
    wire::service_id  service{};
    wire::instance_id instance{};
    std::uint8_t      major_version{};
    std::uint32_t     minor_version{};

    sd_phase phase{sd_phase::down};
    time_point next_send{};
    std::uint32_t repetition_count{0};
};

// Service Discovery state machine.
// Manages offer and find states, produces SD messages via tick().
class discovery {
public:
    explicit discovery(sd_config config = {}) : config_{config} {}

    // Start offering a service.
    void start_offer(wire::service_id service, wire::instance_id instance,
                     std::uint8_t major, std::uint32_t minor,
                     std::vector<sd_option> options = {},
                     time_point now = clock_t::now()) {
        offer_state state;
        state.service = service;
        state.instance = instance;
        state.major_version = major;
        state.minor_version = minor;
        state.options = std::move(options);
        state.phase = sd_phase::initial_wait;
        state.next_send = now + config_.initial_delay_min;
        state.repetition_count = 0;
        offers_.push_back(std::move(state));
    }

    // Stop offering a service. Queues a StopOffer (TTL=0) to send on next tick.
    void stop_offer(wire::service_id service, wire::instance_id instance) {
        for (auto it = offers_.begin(); it != offers_.end(); ++it) {
            if (it->service == service && it->instance == instance) {
                // Queue a stop-offer entry for next tick
                pending_stop_offers_.push_back(*it);
                offers_.erase(it);
                return;
            }
        }
    }

    // Start finding a service.
    void start_find(wire::service_id service, wire::instance_id instance,
                    std::uint8_t major, std::uint32_t minor,
                    time_point now = clock_t::now()) {
        find_state state;
        state.service = service;
        state.instance = instance;
        state.major_version = major;
        state.minor_version = minor;
        state.phase = sd_phase::initial_wait;
        state.next_send = now + config_.initial_delay_min;
        state.repetition_count = 0;
        finds_.push_back(std::move(state));
    }

    // Stop finding a service.
    void stop_find(wire::service_id service, wire::instance_id instance) {
        std::erase_if(finds_, [&](const find_state& f) {
            return f.service == service && f.instance == instance;
        });
    }

    // Advance the state machine. Returns SD messages to send.
    // Call this periodically (e.g. from the event loop timer).
    std::vector<sd_message> tick(time_point now = clock_t::now()) {
        std::vector<sd_message> messages;

        // Collect entries to send
        std::vector<sd_entry> offer_entries;
        std::vector<sd_option> offer_options;

        // Process stop-offers first
        for (const auto& s : pending_stop_offers_) {
            offer_entries.push_back(make_stop_offer(
                s.service, s.instance, s.major_version, s.minor_version));
        }
        pending_stop_offers_.clear();

        // Process active offers
        for (auto& state : offers_) {
            if (state.phase == sd_phase::down) continue;
            if (now < state.next_send) continue;

            auto entry = make_offer_service(
                state.service, state.instance,
                state.major_version, state.minor_version,
                config_.offer_ttl);

            // Set option indices if the offer has options
            if (!state.options.empty()) {
                entry.index_1st = static_cast<std::uint8_t>(offer_options.size());
                entry.num_options_1st = static_cast<std::uint8_t>(state.options.size());
                for (const auto& opt : state.options) {
                    offer_options.push_back(opt);
                }
            }

            offer_entries.push_back(entry);
            advance_phase(state, now);
        }

        if (!offer_entries.empty()) {
            sd_message msg;
            msg.flags = {.reboot = reboot_flag_, .unicast = true};
            msg.entries = std::move(offer_entries);
            msg.options = std::move(offer_options);
            messages.push_back(std::move(msg));
        }

        // Process finds
        std::vector<sd_entry> find_entries;
        for (auto& state : finds_) {
            if (state.phase == sd_phase::down) continue;
            if (now < state.next_send) continue;

            find_entries.push_back(make_find_service(
                state.service, state.instance,
                state.major_version, state.minor_version,
                config_.find_ttl));

            advance_find_phase(state, now);
        }

        if (!find_entries.empty()) {
            sd_message msg;
            msg.flags = {.reboot = reboot_flag_, .unicast = true};
            msg.entries = std::move(find_entries);
            messages.push_back(std::move(msg));
        }

        return messages;
    }

    // Set the reboot flag (should be true on startup, cleared after initial phase).
    void set_reboot_flag(bool v) noexcept { reboot_flag_ = v; }

    // Query state
    const std::vector<offer_state>& offers() const noexcept { return offers_; }
    const std::vector<find_state>& finds() const noexcept { return finds_; }

private:
    void advance_phase(offer_state& s, time_point now) {
        switch (s.phase) {
        case sd_phase::initial_wait:
            s.phase = sd_phase::repetition;
            s.repetition_count = 1;
            s.next_send = now + config_.repetition_base_delay;
            break;
        case sd_phase::repetition:
            ++s.repetition_count;
            if (s.repetition_count >= config_.repetition_max) {
                s.phase = sd_phase::main;
                s.next_send = now + config_.cyclic_offer_delay;
            } else {
                // Exponential backoff: base * 2^(count-1)
                auto delay = config_.repetition_base_delay *
                    (1u << (s.repetition_count - 1));
                s.next_send = now + delay;
            }
            break;
        case sd_phase::main:
            s.next_send = now + config_.cyclic_offer_delay;
            break;
        case sd_phase::down:
            break;
        }
    }

    void advance_find_phase(find_state& s, time_point now) {
        switch (s.phase) {
        case sd_phase::initial_wait:
            s.phase = sd_phase::repetition;
            s.repetition_count = 1;
            s.next_send = now + config_.repetition_base_delay;
            break;
        case sd_phase::repetition:
            ++s.repetition_count;
            if (s.repetition_count >= config_.repetition_max) {
                // Find stops after repetition phase (no main phase)
                s.phase = sd_phase::down;
            } else {
                auto delay = config_.repetition_base_delay *
                    (1u << (s.repetition_count - 1));
                s.next_send = now + delay;
            }
            break;
        default:
            break;
        }
    }

    sd_config config_;
    bool reboot_flag_{true};

    std::vector<offer_state> offers_;
    std::vector<offer_state> pending_stop_offers_;
    std::vector<find_state> finds_;
};

} // namespace bsomeip::sd
