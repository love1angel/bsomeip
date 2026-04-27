// SPDX-License-Identifier: MIT
// io_uring context — owns the ring fd and mapped memory.
// Provides SQE acquisition, submission, and CQE reaping.
// Platform: Linux only (requires kernel 5.1+).
#pragma once

#if !defined(__linux__)
#error "io_uring is Linux-only. This header requires __linux__."
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <system_error>
#include <utility>

#include <sys/mman.h>
#include <linux/io_uring.h>

#include <bsomeip/io/detail/uring_syscall.hpp>

namespace bsomeip::io {

// Owns a single io_uring instance.
// Not thread-safe — use one per thread or protect externally.
class uring_context {
public:
    explicit uring_context(unsigned entries = 256) {
        struct io_uring_params params{};
        // Request kernel-side SQ polling if available, otherwise standard mode
        // params.flags = IORING_SETUP_SQPOLL; // opt-in later

        ring_fd_ = detail::io_uring_setup(entries, &params);
        if (ring_fd_ < 0) {
            throw std::system_error(-ring_fd_, std::system_category(),
                                    "io_uring_setup");
        }

        sq_entries_ = params.sq_entries;
        cq_entries_ = params.cq_entries;
        features_   = params.features;

        // Map submission queue ring
        ring_.sq_size = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
        ring_.sq_ptr = mmap(nullptr, ring_.sq_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring_fd_,
                            IORING_OFF_SQ_RING);
        if (ring_.sq_ptr == MAP_FAILED) {
            close(ring_fd_);
            throw std::system_error(errno, std::system_category(),
                                    "mmap sq_ring");
        }

        // Map completion queue ring
        if (features_ & IORING_FEAT_SINGLE_MMAP) {
            ring_.cq_ptr = ring_.sq_ptr;
            ring_.cq_size = ring_.sq_size;
        } else {
            ring_.cq_size = params.cq_off.cqes +
                            params.cq_entries * sizeof(struct io_uring_cqe);
            ring_.cq_ptr = mmap(nullptr, ring_.cq_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE, ring_fd_,
                                IORING_OFF_CQ_RING);
            if (ring_.cq_ptr == MAP_FAILED) {
                detail::unmap_ring(ring_);
                close(ring_fd_);
                throw std::system_error(errno, std::system_category(),
                                        "mmap cq_ring");
            }
        }

        // Map SQE array
        ring_.sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);
        ring_.sqes = static_cast<struct io_uring_sqe*>(
            mmap(nullptr, ring_.sqes_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, ring_fd_,
                 IORING_OFF_SQES));
        if (ring_.sqes == MAP_FAILED) {
            detail::unmap_ring(ring_);
            close(ring_fd_);
            throw std::system_error(errno, std::system_category(),
                                    "mmap sqes");
        }

        // Cache ring pointers
        auto* sq_base = static_cast<std::byte*>(ring_.sq_ptr);
        sq_head_    = reinterpret_cast<volatile std::uint32_t*>(sq_base + params.sq_off.head);
        sq_tail_    = reinterpret_cast<volatile std::uint32_t*>(sq_base + params.sq_off.tail);
        sq_mask_    = *reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.ring_mask);
        sq_array_   = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.array);
        sq_flags_   = reinterpret_cast<volatile std::uint32_t*>(sq_base + params.sq_off.flags);

        auto* cq_base = static_cast<std::byte*>(ring_.cq_ptr);
        cq_head_    = reinterpret_cast<volatile std::uint32_t*>(cq_base + params.cq_off.head);
        cq_tail_    = reinterpret_cast<volatile std::uint32_t*>(cq_base + params.cq_off.tail);
        cq_mask_    = *reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.ring_mask);
        cqes_       = reinterpret_cast<struct io_uring_cqe*>(cq_base + params.cq_off.cqes);
    }

    ~uring_context() {
        if (ring_fd_ >= 0) {
            detail::unmap_ring(ring_);
            close(ring_fd_);
        }
    }

    // Non-copyable, movable
    uring_context(const uring_context&) = delete;
    uring_context& operator=(const uring_context&) = delete;

    uring_context(uring_context&& o) noexcept
        : ring_fd_{std::exchange(o.ring_fd_, -1)},
          ring_{std::exchange(o.ring_, {})},
          sq_entries_{o.sq_entries_}, cq_entries_{o.cq_entries_},
          features_{o.features_},
          sq_head_{o.sq_head_}, sq_tail_{o.sq_tail_},
          sq_mask_{o.sq_mask_}, sq_array_{o.sq_array_}, sq_flags_{o.sq_flags_},
          cq_head_{o.cq_head_}, cq_tail_{o.cq_tail_},
          cq_mask_{o.cq_mask_}, cqes_{o.cqes_} {}

    uring_context& operator=(uring_context&& o) noexcept {
        if (this != &o) {
            if (ring_fd_ >= 0) {
                detail::unmap_ring(ring_);
                close(ring_fd_);
            }
            ring_fd_ = std::exchange(o.ring_fd_, -1);
            ring_ = std::exchange(o.ring_, {});
            sq_entries_ = o.sq_entries_;
            cq_entries_ = o.cq_entries_;
            features_ = o.features_;
            sq_head_ = o.sq_head_;
            sq_tail_ = o.sq_tail_;
            sq_mask_ = o.sq_mask_;
            sq_array_ = o.sq_array_;
            sq_flags_ = o.sq_flags_;
            cq_head_ = o.cq_head_;
            cq_tail_ = o.cq_tail_;
            cq_mask_ = o.cq_mask_;
            cqes_ = o.cqes_;
        }
        return *this;
    }

    // ---- SQE acquisition ----

    // Get the next available SQE, or nullptr if the SQ is full.
    struct io_uring_sqe* get_sqe() noexcept {
        std::uint32_t head = detail::sq_head_load(sq_head_);
        std::uint32_t tail = sq_tail_local_;
        if (tail - head >= sq_entries_) {
            return nullptr; // SQ full
        }
        std::uint32_t idx = tail & sq_mask_;
        auto* sqe = &ring_.sqes[idx];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array_[idx] = idx;
        sq_tail_local_ = tail + 1;
        return sqe;
    }

    // ---- Submission ----

    // Submit all queued SQEs to the kernel.
    // Returns the number of SQEs submitted, or -errno on error.
    int submit() noexcept {
        std::uint32_t to_submit = sq_tail_local_ - detail::sq_head_load(sq_tail_);
        if (to_submit == 0) return 0;
        detail::sq_tail_store(sq_tail_, sq_tail_local_);
        int ret = detail::io_uring_enter(ring_fd_, to_submit, 0, 0);
        if (ret < 0) return -errno;
        return ret;
    }

    // Submit and wait for at least `min_complete` CQEs.
    int submit_and_wait(unsigned min_complete) noexcept {
        std::uint32_t to_submit = sq_tail_local_ - detail::sq_head_load(sq_tail_);
        detail::sq_tail_store(sq_tail_, sq_tail_local_);
        int ret = detail::io_uring_enter(ring_fd_, to_submit, min_complete,
                                         IORING_ENTER_GETEVENTS);
        if (ret < 0) return -errno;
        return ret;
    }

    // ---- CQE reaping ----

    // Peek at the next CQE without consuming it. Returns nullptr if none.
    struct io_uring_cqe* peek_cqe() noexcept {
        std::uint32_t head = detail::sq_head_load(cq_head_);
        std::uint32_t tail = detail::cq_tail_load(cq_tail_);
        if (head == tail) return nullptr;
        return &cqes_[head & cq_mask_];
    }

    // Consume the current CQE (advance the CQ head).
    void seen_cqe() noexcept {
        std::uint32_t head = *cq_head_ + 1;
        detail::cq_head_store(cq_head_, head);
    }

    // Process all pending CQEs, calling fn(cqe) for each.
    template <typename F>
    unsigned for_each_cqe(F&& fn) noexcept {
        unsigned count = 0;
        std::uint32_t head = detail::sq_head_load(cq_head_);
        std::uint32_t tail = detail::cq_tail_load(cq_tail_);
        while (head != tail) {
            fn(&cqes_[head & cq_mask_]);
            ++head;
            ++count;
        }
        detail::cq_head_store(cq_head_, head);
        return count;
    }

    int fd() const noexcept { return ring_fd_; }
    unsigned sq_capacity() const noexcept { return sq_entries_; }
    unsigned cq_capacity() const noexcept { return cq_entries_; }

private:
    int ring_fd_ = -1;
    detail::mapped_ring ring_{};

    std::uint32_t sq_entries_ = 0;
    std::uint32_t cq_entries_ = 0;
    std::uint32_t features_   = 0;

    // SQ ring pointers
    volatile std::uint32_t* sq_head_  = nullptr;
    volatile std::uint32_t* sq_tail_  = nullptr;
    std::uint32_t           sq_mask_  = 0;
    std::uint32_t*          sq_array_ = nullptr;
    volatile std::uint32_t* sq_flags_ = nullptr;
    std::uint32_t           sq_tail_local_ = 0; // Local copy of SQ tail

    // CQ ring pointers
    volatile std::uint32_t* cq_head_ = nullptr;
    volatile std::uint32_t* cq_tail_ = nullptr;
    std::uint32_t           cq_mask_ = 0;
    struct io_uring_cqe*    cqes_    = nullptr;
};

} // namespace bsomeip::io
