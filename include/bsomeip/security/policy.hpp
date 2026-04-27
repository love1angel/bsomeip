// SPDX-License-Identifier: MIT
// Security policy: allow/deny rules for SOME/IP access control.
//
// Rules match by (client_id, service_id, instance_id, method_id).
// Wildcards (any_*) match everything. First matching rule wins.
// Default policy: deny-all (must explicitly allow).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/constants.hpp>

namespace bsomeip::security {

// Rule action
enum class action : std::uint8_t {
    allow,
    deny,
};

// A single access control rule.
// Fields set to any_* act as wildcards.
struct rule {
    wire::client_id   client   = wire::any_client;
    wire::service_id  service  = wire::any_service;
    wire::instance_id instance = wire::any_instance;
    wire::method_id   method   = wire::any_method;
    action            act      = action::deny;

    // Match this rule against a specific request.
    constexpr bool matches(wire::client_id c, wire::service_id s,
                           wire::instance_id i, wire::method_id m) const noexcept {
        return (client == wire::any_client     || client == c) &&
               (service == wire::any_service   || service == s) &&
               (instance == wire::any_instance || instance == i) &&
               (method == wire::any_method     || method == m);
    }
};

// Convenience builders
constexpr rule allow_all() noexcept {
    return {wire::any_client, wire::any_service, wire::any_instance,
            wire::any_method, action::allow};
}

constexpr rule deny_all() noexcept {
    return {wire::any_client, wire::any_service, wire::any_instance,
            wire::any_method, action::deny};
}

constexpr rule allow_client_service(wire::client_id client,
                                     wire::service_id service) noexcept {
    return {client, service, wire::any_instance, wire::any_method,
            action::allow};
}

constexpr rule deny_client(wire::client_id client) noexcept {
    return {client, wire::any_service, wire::any_instance, wire::any_method,
            action::deny};
}

constexpr rule allow_service_method(wire::service_id service,
                                     wire::method_id method) noexcept {
    return {wire::any_client, service, wire::any_instance, method,
            action::allow};
}

// ============================================================
// Policy: ordered list of rules, first match wins.
// Thread-safety: immutable after construction. Build the policy,
// then hand it to the enforcer. No mutation during enforcement.
// ============================================================

class policy {
public:
    policy() = default;

    // Add a rule (appended; order matters for first-match).
    void add(rule r) { rules_.push_back(r); }

    // Evaluate the policy for a given request.
    // Returns the action of the first matching rule, or deny if none match.
    constexpr action evaluate(wire::client_id client,
                               wire::service_id service,
                               wire::instance_id instance,
                               wire::method_id method) const noexcept {
        for (const auto& r : rules_) {
            if (r.matches(client, service, instance, method))
                return r.act;
        }
        return action::deny;  // default deny
    }

    // Check if a request is allowed.
    constexpr bool is_allowed(wire::client_id client,
                               wire::service_id service,
                               wire::instance_id instance,
                               wire::method_id method) const noexcept {
        return evaluate(client, service, instance, method) == action::allow;
    }

    std::size_t rule_count() const noexcept { return rules_.size(); }
    const std::vector<rule>& rules() const noexcept { return rules_; }

    void clear() { rules_.clear(); }

private:
    std::vector<rule> rules_;
};

} // namespace bsomeip::security
