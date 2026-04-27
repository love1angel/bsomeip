// SPDX-License-Identifier: MIT
// Low-level io_uring syscall interface — no liburing dependency.
// Platform: Linux only (requires kernel 5.1+).
#pragma once

#if !defined(__linux__)
#error "io_uring is Linux-only. This header requires __linux__."
#endif

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/io_uring.h>

namespace bsomeip::io::detail {

// ---- Syscall wrappers ----

inline int io_uring_setup(unsigned entries, struct io_uring_params* p) noexcept {
    return static_cast<int>(syscall(__NR_io_uring_setup, entries, p));
}

inline int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                          unsigned flags, void* sig = nullptr) noexcept {
    return static_cast<int>(
        syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, 0));
}

inline int io_uring_register(int fd, unsigned opcode, void* arg,
                             unsigned nr_args) noexcept {
    return static_cast<int>(
        syscall(__NR_io_uring_register, fd, opcode, arg, nr_args));
}

// ---- Ring memory mapping ----

struct mapped_ring {
    void* sq_ptr = nullptr;
    std::size_t sq_size = 0;
    void* cq_ptr = nullptr;
    std::size_t cq_size = 0;
    struct io_uring_sqe* sqes = nullptr;
    std::size_t sqes_size = 0;
};

// Unmap all ring memory
inline void unmap_ring(mapped_ring& ring) noexcept {
    if (ring.sq_ptr) {
        munmap(ring.sq_ptr, ring.sq_size);
        ring.sq_ptr = nullptr;
    }
    // CQ may share mapping with SQ (IORING_FEAT_SINGLE_MMAP)
    if (ring.cq_ptr && ring.cq_ptr != ring.sq_ptr) {
        munmap(ring.cq_ptr, ring.cq_size);
    }
    ring.cq_ptr = nullptr;
    if (ring.sqes) {
        munmap(ring.sqes, ring.sqes_size);
        ring.sqes = nullptr;
    }
}

// ---- Submission queue helpers ----

// Read the kernel's SQ head (acquire barrier)
inline std::uint32_t sq_head_load(const volatile std::uint32_t* head) noexcept {
    return __atomic_load_n(head, __ATOMIC_ACQUIRE);
}

// Write the SQ tail (release barrier)
inline void sq_tail_store(volatile std::uint32_t* tail, std::uint32_t val) noexcept {
    __atomic_store_n(tail, val, __ATOMIC_RELEASE);
}

// Read the kernel's CQ tail (acquire barrier)
inline std::uint32_t cq_tail_load(const volatile std::uint32_t* tail) noexcept {
    return __atomic_load_n(tail, __ATOMIC_ACQUIRE);
}

// Write the CQ head (release barrier)
inline void cq_head_store(volatile std::uint32_t* head, std::uint32_t val) noexcept {
    __atomic_store_n(head, val, __ATOMIC_RELEASE);
}

} // namespace bsomeip::io::detail
