// SPDX-License-Identifier: MIT
// Security enforcer: sender adaptor that checks policies before dispatch.
//
// Composable in stdexec pipelines:
//   recv_sender | security::enforce(policy) | dispatch()
//
// Extracts (client, service, instance, method) from the SOME/IP header
// and evaluates against the policy. If denied, signals set_error with
// security_error. If allowed, forwards the message downstream.
#pragma once

#include <cstddef>
#include <exception>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <bsomeip/async/execution.hpp>

#include <bsomeip/security/policy.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>

namespace bsomeip::security {

// Error thrown when a message is denied by the security policy.
class security_error : public std::runtime_error {
public:
    security_error(wire::client_id client, wire::service_id service,
                   wire::method_id method)
        : std::runtime_error("security policy denied")
        , client_{client}, service_{service}, method_{method} {}

    wire::client_id  client()  const noexcept { return client_; }
    wire::service_id service() const noexcept { return service_; }
    wire::method_id  method()  const noexcept { return method_; }

private:
    wire::client_id  client_;
    wire::service_id service_;
    wire::method_id  method_;
};

// ============================================================
// enforce_sender: wraps an upstream sender, checks the SOME/IP
// header against a security policy before forwarding downstream.
//
// Upstream produces: std::vector<std::byte> (message buffer)
// Downstream receives: std::vector<std::byte> (if allowed)
// ============================================================

template <typename Upstream>
class enforce_sender {
public:
    using sender_concept = bsomeip::async::sender_tag;
    using completion_signatures = bsomeip::async::completion_signatures<
        bsomeip::async::set_value_t(std::vector<std::byte>),
        bsomeip::async::set_error_t(std::exception_ptr),
        bsomeip::async::set_stopped_t()>;

    enforce_sender(Upstream upstream, const policy* pol,
                   wire::instance_id instance = wire::any_instance) noexcept
        : upstream_{std::move(upstream)}, policy_{pol}, instance_{instance} {}

    template <typename Receiver>
    struct op_state {
        using operation_state_concept = bsomeip::async::operation_state_tag;

        struct inner_receiver {
            using receiver_concept = bsomeip::async::receiver_tag;

            op_state* op_;

            void set_value(std::vector<std::byte> msg) && noexcept {
                if (msg.size() < wire::header_size) {
                    bsomeip::async::set_error(std::move(op_->rcvr_),
                        std::make_exception_ptr(
                            security_error(wire::client_id{0},
                                           wire::service_id{0},
                                           wire::method_id{0})));
                    return;
                }

                wire::header_view hdr{msg.data()};
                auto client  = hdr.client();
                auto service = hdr.service();
                auto method  = hdr.method();

                if (!op_->policy_->is_allowed(client, service,
                                               op_->instance_, method)) {
                    bsomeip::async::set_error(std::move(op_->rcvr_),
                        std::make_exception_ptr(
                            security_error(client, service, method)));
                    return;
                }

                bsomeip::async::set_value(std::move(op_->rcvr_), std::move(msg));
            }

            void set_error(std::exception_ptr e) && noexcept {
                bsomeip::async::set_error(std::move(op_->rcvr_), std::move(e));
            }

            void set_stopped() && noexcept {
                bsomeip::async::set_stopped(std::move(op_->rcvr_));
            }

            auto get_env() const noexcept { return bsomeip::async::empty_env{}; }
        };

        using inner_op_t = decltype(bsomeip::async::connect(
            std::declval<Upstream>(), std::declval<inner_receiver>()));

        const policy* policy_;
        wire::instance_id instance_;
        Receiver rcvr_;
        inner_op_t inner_op_;

        op_state(Upstream upstream, const policy* pol,
                 wire::instance_id instance, Receiver rcvr)
            : policy_{pol}
            , instance_{instance}
            , rcvr_{std::move(rcvr)}
            , inner_op_{bsomeip::async::connect(std::move(upstream),
                                          inner_receiver{this})}
        {}

        void start() & noexcept {
            bsomeip::async::start(inner_op_);
        }
    };

    template <typename Receiver>
    auto connect(Receiver rcvr) && noexcept
        -> op_state<Receiver>
    {
        return {std::move(upstream_), policy_, instance_, std::move(rcvr)};
    }

    auto get_env() const noexcept { return bsomeip::async::empty_env{}; }

private:
    Upstream upstream_;
    const policy* policy_;
    wire::instance_id instance_;
};

// ============================================================
// Pipe-style adaptor: security::enforce(policy)
//
// Usage:
//   auto verified = recv_sender | security::enforce(my_policy);
// ============================================================

struct enforce_adaptor {
    const policy* policy_;
    wire::instance_id instance_ = wire::any_instance;

    template <typename Sender>
    friend auto operator|(Sender&& sender, enforce_adaptor adaptor) {
        return enforce_sender<std::decay_t<Sender>>{
            std::forward<Sender>(sender), adaptor.policy_, adaptor.instance_};
    }
};

inline auto enforce(const policy& pol,
                    wire::instance_id instance = wire::any_instance) {
    return enforce_adaptor{&pol, instance};
}

} // namespace bsomeip::security
