// SPDX-License-Identifier: MIT
// io_uring-based sender/receiver scheduler.
// schedule() returns a sender that completes on the io_uring event loop.
// Platform: Linux only (requires kernel 5.1+).
#pragma once

#if !defined(__linux__)
#error "io_uring is Linux-only. This header requires __linux__."
#endif

#include <cstdint>
#include <atomic>
#include <cassert>
#include <system_error>

#include <linux/io_uring.h>
#include <stdexec/execution.hpp>

#include <bsomeip/io/uring_context.hpp>

namespace bsomeip::io {

class uring_scheduler;

namespace detail {

// Base class for all io_uring operation states.
// Holds the completion callback pointer so CQE processing can dispatch.
struct uring_op_base {
    using callback_t = void (*)(uring_op_base* self, int res, unsigned flags) noexcept;
    callback_t callback_ = nullptr;
};

} // namespace detail

// Event loop: drives the io_uring and dispatches completions.
// Owns a uring_context and processes CQEs, calling operation callbacks.
class uring_event_loop {
public:
    explicit uring_event_loop(unsigned entries = 256)
        : ctx_{entries} {}

    // Run the event loop until stop() is called.
    void run() {
        stop_.store(false, std::memory_order_relaxed);
        while (!stop_.load(std::memory_order_relaxed)) {
            // Submit pending SQEs and wait for at least 1 CQE
            int ret = ctx_.submit_and_wait(1);
            if (ret < 0) {
                if (-ret == EINTR) continue;
                throw std::system_error(-ret, std::system_category(),
                                        "io_uring_enter");
            }
            // Process all available CQEs
            ctx_.for_each_cqe([](struct io_uring_cqe* cqe) {
                auto* op = reinterpret_cast<detail::uring_op_base*>(cqe->user_data);
                if (op && op->callback_) {
                    op->callback_(op, cqe->res, cqe->flags);
                }
            });
        }
    }

    // Run one batch of completions (non-blocking).
    // Returns the number of CQEs processed.
    unsigned run_once() {
        ctx_.submit();
        return ctx_.for_each_cqe([](struct io_uring_cqe* cqe) {
            auto* op = reinterpret_cast<detail::uring_op_base*>(cqe->user_data);
            if (op && op->callback_) {
                op->callback_(op, cqe->res, cqe->flags);
            }
        });
    }

    void stop() noexcept {
        stop_.store(true, std::memory_order_relaxed);
    }

    uring_context& context() noexcept { return ctx_; }
    const uring_context& context() const noexcept { return ctx_; }

    uring_scheduler get_scheduler() noexcept;

private:
    uring_context ctx_;
    std::atomic<bool> stop_{false};
};

// stdexec scheduler backed by io_uring.
// schedule() returns a sender that enqueues a NOP into the ring
// and completes when the CQE arrives.
class uring_scheduler {
    friend class uring_event_loop;

    // Environment that reports the completion scheduler.
    // Uses uring_event_loop* to avoid incomplete-type issue.
    struct env {
        uring_event_loop* loop_;

        auto query(stdexec::get_scheduler_t) const noexcept {
            return uring_scheduler{loop_};
        }
    };

    // Operation state for schedule() sender
    template <class Receiver>
    struct op_state : detail::uring_op_base {
        using operation_state_concept = stdexec::operation_state_tag;

        uring_event_loop* loop_;
        Receiver rcvr_;

        op_state(uring_event_loop* loop, Receiver rcvr) noexcept
            : loop_{loop}, rcvr_{static_cast<Receiver&&>(rcvr)} {
            this->callback_ = &complete;
        }

        void start() & noexcept {
            // Enqueue a NOP into the ring with user_data pointing to this op
            auto* sqe = loop_->context().get_sqe();
            if (!sqe) {
                // SQ full — try to submit and retry once
                loop_->context().submit();
                sqe = loop_->context().get_sqe();
            }
            if (!sqe) {
                stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                    std::make_exception_ptr(
                        std::system_error(EBUSY, std::system_category(),
                                          "io_uring SQ full")));
                return;
            }
            sqe->opcode = IORING_OP_NOP;
            sqe->user_data = reinterpret_cast<std::uint64_t>(
                static_cast<detail::uring_op_base*>(this));
        }

        static void complete(detail::uring_op_base* base, int res,
                             unsigned /*flags*/) noexcept {
            auto* self = static_cast<op_state*>(base);
            if (res < 0) {
                stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                    std::make_exception_ptr(
                        std::system_error(-res, std::system_category())));
            } else {
                stdexec::set_value(static_cast<Receiver&&>(self->rcvr_));
            }
        }
    };

    // Sender returned by schedule()
    struct sender {
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_error_t(std::exception_ptr),
            stdexec::set_stopped_t()>;

        uring_event_loop* loop_;

        template <class Receiver>
        auto connect(Receiver rcvr) const noexcept -> op_state<Receiver> {
            return {loop_, static_cast<Receiver&&>(rcvr)};
        }

        auto get_env() const noexcept -> env {
            return {loop_};
        }
    };

public:
    using scheduler_concept = stdexec::scheduler_tag;

    explicit uring_scheduler(uring_event_loop* loop) noexcept : loop_{loop} {}

    auto schedule() const noexcept -> sender {
        return {loop_};
    }

    auto operator==(const uring_scheduler&) const noexcept -> bool = default;

private:
    uring_event_loop* loop_;
};

inline uring_scheduler uring_event_loop::get_scheduler() noexcept {
    return uring_scheduler{this};
}

} // namespace bsomeip::io
