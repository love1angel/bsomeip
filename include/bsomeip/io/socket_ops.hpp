// SPDX-License-Identifier: MIT
// Async socket operations as sender/receiver senders, backed by io_uring.
// Platform: Linux only.
#pragma once

#if !defined(__linux__)
#error "io_uring socket ops require Linux. This header requires __linux__."
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <linux/io_uring.h>

#include <stdexec/execution.hpp>
#include <bsomeip/io/uring_scheduler.hpp>

namespace bsomeip::io {

// ============================================================
// async_accept — accept a connection on a listening socket
// Completes with: set_value(int client_fd, sockaddr_storage addr)
// ============================================================

namespace detail {

template <class Receiver>
struct accept_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int listen_fd_;
    Receiver rcvr_;
    struct sockaddr_storage addr_{};
    socklen_t addrlen_ = sizeof(addr_);

    accept_op(uring_event_loop* loop, int fd, Receiver rcvr) noexcept
        : loop_{loop}, listen_fd_{fd}, rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_ACCEPT;
        sqe->fd = listen_fd_;
        sqe->addr = reinterpret_cast<std::uint64_t>(&addr_);
        sqe->addr2 = reinterpret_cast<std::uint64_t>(&addrlen_);
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<accept_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_),
                               res, self->addr_);
        }
    }
};

} // namespace detail

struct async_accept_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(int, struct sockaddr_storage),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int listen_fd_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::accept_op<Receiver>{loop_, listen_fd_,
                                           static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept {
        return stdexec::empty_env{};
    }
};

inline auto async_accept(uring_event_loop& loop, int listen_fd) {
    return async_accept_sender{&loop, listen_fd};
}

// ============================================================
// async_recv — receive data into a buffer
// Completes with: set_value(std::size_t bytes_read)
// ============================================================

namespace detail {

template <class Receiver>
struct recv_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int fd_;
    std::span<std::byte> buf_;
    Receiver rcvr_;

    recv_op(uring_event_loop* loop, int fd, std::span<std::byte> buf,
            Receiver rcvr) noexcept
        : loop_{loop}, fd_{fd}, buf_{buf},
          rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_RECV;
        sqe->fd = fd_;
        sqe->addr = reinterpret_cast<std::uint64_t>(buf_.data());
        sqe->len = static_cast<std::uint32_t>(buf_.size());
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<recv_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_),
                               static_cast<std::size_t>(res));
        }
    }
};

} // namespace detail

struct async_recv_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int fd_;
    std::span<std::byte> buf_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::recv_op<Receiver>{loop_, fd_, buf_,
                                         static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept { return stdexec::empty_env{}; }
};

inline auto async_recv(uring_event_loop& loop, int fd,
                       std::span<std::byte> buf) {
    return async_recv_sender{&loop, fd, buf};
}

// ============================================================
// async_send — send data from a buffer
// Completes with: set_value(std::size_t bytes_written)
// ============================================================

namespace detail {

template <class Receiver>
struct send_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int fd_;
    std::span<const std::byte> buf_;
    Receiver rcvr_;

    send_op(uring_event_loop* loop, int fd, std::span<const std::byte> buf,
            Receiver rcvr) noexcept
        : loop_{loop}, fd_{fd}, buf_{buf},
          rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_SEND;
        sqe->fd = fd_;
        sqe->addr = reinterpret_cast<std::uint64_t>(buf_.data());
        sqe->len = static_cast<std::uint32_t>(buf_.size());
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<send_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_),
                               static_cast<std::size_t>(res));
        }
    }
};

} // namespace detail

struct async_send_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int fd_;
    std::span<const std::byte> buf_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::send_op<Receiver>{loop_, fd_, buf_,
                                         static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept { return stdexec::empty_env{}; }
};

inline auto async_send(uring_event_loop& loop, int fd,
                       std::span<const std::byte> buf) {
    return async_send_sender{&loop, fd, buf};
}

// ============================================================
// async_recvfrom — receive UDP datagram
// Completes with: set_value(std::size_t bytes, sockaddr_storage from)
// ============================================================

namespace detail {

template <class Receiver>
struct recvfrom_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int fd_;
    std::span<std::byte> buf_;
    Receiver rcvr_;
    struct msghdr msg_{};
    struct iovec iov_{};
    struct sockaddr_storage from_addr_{};

    recvfrom_op(uring_event_loop* loop, int fd, std::span<std::byte> buf,
                Receiver rcvr) noexcept
        : loop_{loop}, fd_{fd}, buf_{buf},
          rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
        iov_.iov_base = buf_.data();
        iov_.iov_len = buf_.size();
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
        msg_.msg_name = &from_addr_;
        msg_.msg_namelen = sizeof(from_addr_);
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_RECVMSG;
        sqe->fd = fd_;
        sqe->addr = reinterpret_cast<std::uint64_t>(&msg_);
        sqe->len = 1;
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<recvfrom_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_),
                               static_cast<std::size_t>(res),
                               self->from_addr_);
        }
    }
};

} // namespace detail

struct async_recvfrom_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t, struct sockaddr_storage),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int fd_;
    std::span<std::byte> buf_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::recvfrom_op<Receiver>{loop_, fd_, buf_,
                                             static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept { return stdexec::empty_env{}; }
};

inline auto async_recvfrom(uring_event_loop& loop, int fd,
                           std::span<std::byte> buf) {
    return async_recvfrom_sender{&loop, fd, buf};
}

// ============================================================
// async_sendto — send UDP datagram to a destination
// Completes with: set_value(std::size_t bytes_sent)
// ============================================================

namespace detail {

template <class Receiver>
struct sendto_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int fd_;
    std::span<const std::byte> buf_;
    struct sockaddr_storage dest_{};
    socklen_t dest_len_;
    Receiver rcvr_;
    struct msghdr msg_{};
    struct iovec iov_{};

    sendto_op(uring_event_loop* loop, int fd, std::span<const std::byte> buf,
              const struct sockaddr* dest, socklen_t dest_len,
              Receiver rcvr) noexcept
        : loop_{loop}, fd_{fd}, buf_{buf}, dest_len_{dest_len},
          rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
        std::memcpy(&dest_, dest, dest_len);
        iov_.iov_base = const_cast<std::byte*>(buf_.data());
        iov_.iov_len = buf_.size();
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
        msg_.msg_name = &dest_;
        msg_.msg_namelen = dest_len_;
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_SENDMSG;
        sqe->fd = fd_;
        sqe->addr = reinterpret_cast<std::uint64_t>(&msg_);
        sqe->len = 1;
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<sendto_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_),
                               static_cast<std::size_t>(res));
        }
    }
};

} // namespace detail

struct async_sendto_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int fd_;
    std::span<const std::byte> buf_;
    struct sockaddr_storage dest_;
    socklen_t dest_len_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::sendto_op<Receiver>{
            loop_, fd_, buf_,
            reinterpret_cast<const struct sockaddr*>(&dest_), dest_len_,
            static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept { return stdexec::empty_env{}; }
};

inline auto async_sendto(uring_event_loop& loop, int fd,
                         std::span<const std::byte> buf,
                         const struct sockaddr* dest, socklen_t dest_len) {
    async_sendto_sender s{&loop, fd, buf, {}, dest_len};
    std::memcpy(&s.dest_, dest, dest_len);
    return s;
}

// ============================================================
// async_connect — connect a socket to a remote address
// Completes with: set_value()
// ============================================================

namespace detail {

template <class Receiver>
struct connect_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int fd_;
    struct sockaddr_storage addr_{};
    socklen_t addrlen_;
    Receiver rcvr_;

    connect_op(uring_event_loop* loop, int fd,
               const struct sockaddr* addr, socklen_t addrlen,
               Receiver rcvr) noexcept
        : loop_{loop}, fd_{fd}, addrlen_{addrlen},
          rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
        std::memcpy(&addr_, addr, addrlen);
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_CONNECT;
        sqe->fd = fd_;
        sqe->addr = reinterpret_cast<std::uint64_t>(&addr_);
        sqe->off = addrlen_;
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<connect_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_));
        }
    }
};

} // namespace detail

struct async_connect_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int fd_;
    struct sockaddr_storage addr_;
    socklen_t addrlen_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::connect_op<Receiver>{
            loop_, fd_,
            reinterpret_cast<const struct sockaddr*>(&addr_), addrlen_,
            static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept { return stdexec::empty_env{}; }
};

inline auto async_connect(uring_event_loop& loop, int fd,
                          const struct sockaddr* addr, socklen_t addrlen) {
    async_connect_sender s{&loop, fd, {}, addrlen};
    std::memcpy(&s.addr_, addr, addrlen);
    return s;
}

// ============================================================
// async_close — close a file descriptor via io_uring
// Completes with: set_value()
// ============================================================

namespace detail {

template <class Receiver>
struct close_op : uring_op_base {
    using operation_state_concept = stdexec::operation_state_tag;

    uring_event_loop* loop_;
    int fd_;
    Receiver rcvr_;

    close_op(uring_event_loop* loop, int fd, Receiver rcvr) noexcept
        : loop_{loop}, fd_{fd}, rcvr_{static_cast<Receiver&&>(rcvr)} {
        this->callback_ = &complete;
    }

    void start() & noexcept {
        auto* sqe = loop_->context().get_sqe();
        if (!sqe) { loop_->context().submit(); sqe = loop_->context().get_sqe(); }
        if (!sqe) {
            stdexec::set_error(static_cast<Receiver&&>(rcvr_),
                std::make_exception_ptr(
                    std::system_error(EBUSY, std::system_category())));
            return;
        }
        sqe->opcode = IORING_OP_CLOSE;
        sqe->fd = fd_;
        sqe->user_data = reinterpret_cast<std::uint64_t>(
            static_cast<uring_op_base*>(this));
    }

    static void complete(uring_op_base* base, int res, unsigned) noexcept {
        auto* self = static_cast<close_op*>(base);
        if (res < 0) {
            stdexec::set_error(static_cast<Receiver&&>(self->rcvr_),
                std::make_exception_ptr(
                    std::system_error(-res, std::system_category())));
        } else {
            stdexec::set_value(static_cast<Receiver&&>(self->rcvr_));
        }
    }
};

} // namespace detail

struct async_close_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    uring_event_loop* loop_;
    int fd_;

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept {
        return detail::close_op<Receiver>{loop_, fd_,
                                          static_cast<Receiver&&>(rcvr)};
    }

    auto get_env() const noexcept { return stdexec::empty_env{}; }
};

inline auto async_close(uring_event_loop& loop, int fd) {
    return async_close_sender{&loop, fd};
}

} // namespace bsomeip::io
