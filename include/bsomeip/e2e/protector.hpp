// SPDX-License-Identifier: MIT
// E2E protector: sender adaptor for per-message E2E protection.
//
// Composable in stdexec pipelines:
//   message | e2e::protect(profile_cfg) | send()
//   recv()  | e2e::check(profile_cfg)   | dispatch()
//
// The protector maintains counter state and applies CRC to the payload
// region of the message. Uses profile_01/02/04/07 from profile.hpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <bsomeip/async/execution.hpp>

#include <bsomeip/e2e/profile.hpp>
#include <bsomeip/wire/header.hpp>

namespace bsomeip::e2e {

// ============================================================
// e2e_state: per-data-ID counter tracking for protect and check.
// Thread-safety: single-threaded per data-ID (typical SOME/IP usage).
// ============================================================

template <typename Profile>
struct e2e_state {
    using config_type = typename Profile::config_type;

    config_type config;

    // Counter for outgoing messages (protect) — starts at 1 so first
    // check (with recv_counter=0) sees delta=1 → ok.
    std::uint32_t send_counter{1};

    // Last received counter (check)
    std::uint32_t recv_counter{0};

    explicit e2e_state(config_type cfg) : config{std::move(cfg)} {}
};

// ============================================================
// protect_sender: wraps an upstream sender, applies E2E protection
// to the payload before forwarding the value downstream.
//
// Upstream must produce: std::vector<std::byte> (owning message buffer)
// Downstream receives:   std::vector<std::byte> (with E2E header applied)
// ============================================================

template <typename Profile, typename Upstream>
class protect_sender {
public:
    using sender_concept = bsomeip::async::sender_tag;
    using completion_signatures = bsomeip::async::completion_signatures<
        bsomeip::async::set_value_t(std::vector<std::byte>),
        bsomeip::async::set_error_t(std::exception_ptr),
        bsomeip::async::set_stopped_t()>;

    protect_sender(Upstream upstream, e2e_state<Profile>* state) noexcept
        : upstream_{std::move(upstream)}, state_{state} {}

    template <typename Receiver>
    struct op_state {
        using operation_state_concept = bsomeip::async::operation_state_tag;

        struct inner_receiver {
            using receiver_concept = bsomeip::async::receiver_tag;

            op_state* op_;

            void set_value(std::vector<std::byte> msg) && noexcept {
                // Apply E2E protection to the payload portion
                if (msg.size() > wire::header_size) {
                    auto payload = std::span{msg}.subspan(wire::header_size);

                    if constexpr (std::is_same_v<Profile, profile_01> ||
                                  std::is_same_v<Profile, profile_02>) {
                        Profile::protect(payload, op_->state_->config,
                                         static_cast<std::uint8_t>(
                                             op_->state_->send_counter & 0x0F));
                    } else if constexpr (std::is_same_v<Profile, profile_04>) {
                        Profile::protect(payload, op_->state_->config,
                                         static_cast<std::uint16_t>(
                                             op_->state_->send_counter));
                    } else {
                        Profile::protect(payload, op_->state_->config,
                                         op_->state_->send_counter);
                    }
                    op_->state_->send_counter++;

                    // Update SOME/IP header length field
                    wire::header_view hdr{msg.data()};
                    hdr.set_payload_length(
                        static_cast<wire::length_t>(payload.size()));
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

        e2e_state<Profile>* state_;
        Receiver rcvr_;
        inner_op_t inner_op_;

        op_state(Upstream upstream, e2e_state<Profile>* state, Receiver rcvr)
            : state_{state}
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
        return {std::move(upstream_), state_, std::move(rcvr)};
    }

    auto get_env() const noexcept { return bsomeip::async::empty_env{}; }

private:
    Upstream upstream_;
    e2e_state<Profile>* state_;
};

// ============================================================
// check_sender: wraps an upstream sender, verifies E2E protection
// on the payload and forwards if valid.
//
// Upstream must produce: std::vector<std::byte> (raw message buffer)
// Downstream receives:   std::vector<std::byte> (verified message)
//   On CRC failure → set_error with e2e_error.
// ============================================================

class e2e_error : public std::runtime_error {
public:
    e2e_error(e2e_result result)
        : std::runtime_error("E2E check failed"), result_{result} {}
    e2e_result result() const noexcept { return result_; }
private:
    e2e_result result_;
};

template <typename Profile, typename Upstream>
class check_sender {
public:
    using sender_concept = bsomeip::async::sender_tag;
    using completion_signatures = bsomeip::async::completion_signatures<
        bsomeip::async::set_value_t(std::vector<std::byte>),
        bsomeip::async::set_error_t(std::exception_ptr),
        bsomeip::async::set_stopped_t()>;

    check_sender(Upstream upstream, e2e_state<Profile>* state) noexcept
        : upstream_{std::move(upstream)}, state_{state} {}

    template <typename Receiver>
    struct op_state {
        using operation_state_concept = bsomeip::async::operation_state_tag;

        struct inner_receiver {
            using receiver_concept = bsomeip::async::receiver_tag;

            op_state* op_;

            void set_value(std::vector<std::byte> msg) && noexcept {
                if (msg.size() <= wire::header_size) {
                    bsomeip::async::set_error(std::move(op_->rcvr_),
                        std::make_exception_ptr(e2e_error(e2e_result::error)));
                    return;
                }

                auto payload = std::span<const std::byte>{msg}.subspan(
                    wire::header_size);

                e2e_result res;
                if constexpr (std::is_same_v<Profile, profile_01> ||
                              std::is_same_v<Profile, profile_02>) {
                    std::uint8_t cnt =
                        static_cast<std::uint8_t>(op_->state_->recv_counter);
                    res = Profile::check(payload, op_->state_->config, cnt);
                    op_->state_->recv_counter = cnt;
                } else if constexpr (std::is_same_v<Profile, profile_04>) {
                    std::uint16_t cnt =
                        static_cast<std::uint16_t>(op_->state_->recv_counter);
                    res = Profile::check(payload, op_->state_->config, cnt);
                    op_->state_->recv_counter = cnt;
                } else {
                    res = Profile::check(payload, op_->state_->config,
                                         op_->state_->recv_counter);
                }

                if (res == e2e_result::ok || res == e2e_result::no_new_data) {
                    bsomeip::async::set_value(std::move(op_->rcvr_), std::move(msg));
                } else {
                    bsomeip::async::set_error(std::move(op_->rcvr_),
                        std::make_exception_ptr(e2e_error(res)));
                }
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

        e2e_state<Profile>* state_;
        Receiver rcvr_;
        inner_op_t inner_op_;

        op_state(Upstream upstream, e2e_state<Profile>* state, Receiver rcvr)
            : state_{state}
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
        return {std::move(upstream_), state_, std::move(rcvr)};
    }

    auto get_env() const noexcept { return bsomeip::async::empty_env{}; }

private:
    Upstream upstream_;
    e2e_state<Profile>* state_;
};

// ============================================================
// Pipe-style adaptors: e2e::protect(state) and e2e::check(state)
//
// Usage:
//   auto msg_sender = stdexec::just(msg_buffer);
//   auto protected_msg = msg_sender | e2e::protect(my_state);
//   auto verified_msg  = recv_sender | e2e::check(my_state);
// ============================================================

template <typename Profile>
struct protect_adaptor {
    e2e_state<Profile>* state_;

    template <typename Sender>
    auto operator()(Sender&& sender) && {
        return protect_sender<Profile, std::decay_t<Sender>>{
            std::forward<Sender>(sender), state_};
    }

    template <typename Sender>
    friend auto operator|(Sender&& sender, protect_adaptor adaptor) {
        return protect_sender<Profile, std::decay_t<Sender>>{
            std::forward<Sender>(sender), adaptor.state_};
    }
};

template <typename Profile>
struct check_adaptor {
    e2e_state<Profile>* state_;

    template <typename Sender>
    auto operator()(Sender&& sender) && {
        return check_sender<Profile, std::decay_t<Sender>>{
            std::forward<Sender>(sender), state_};
    }

    template <typename Sender>
    friend auto operator|(Sender&& sender, check_adaptor adaptor) {
        return check_sender<Profile, std::decay_t<Sender>>{
            std::forward<Sender>(sender), adaptor.state_};
    }
};

template <typename Profile>
auto protect(e2e_state<Profile>& state) {
    return protect_adaptor<Profile>{&state};
}

template <typename Profile>
auto check(e2e_state<Profile>& state) {
    return check_adaptor<Profile>{&state};
}

} // namespace bsomeip::e2e
