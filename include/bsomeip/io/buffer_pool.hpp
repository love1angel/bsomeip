// SPDX-License-Identifier: MIT
// Pre-allocated buffer pool for zero-copy I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <cassert>

namespace bsomeip::io {

// Fixed-size buffer pool. Allocates a contiguous block up front;
// individual buffers are handed out and returned without malloc.
class buffer_pool {
public:
    // Create a pool of `count` buffers, each `buf_size` bytes.
    explicit buffer_pool(std::size_t count = 256,
                         std::size_t buf_size = 8192)
        : buf_size_{buf_size} {
        storage_.resize(count * buf_size);
        free_list_.reserve(count);
        for (std::size_t i = count; i > 0; --i) {
            free_list_.push_back(static_cast<std::uint32_t>(i - 1));
        }
    }

    // Acquire a buffer. Returns an empty span if pool is exhausted.
    std::span<std::byte> acquire() noexcept {
        if (free_list_.empty()) return {};
        auto idx = free_list_.back();
        free_list_.pop_back();
        return {storage_.data() + idx * buf_size_, buf_size_};
    }

    // Release a buffer back to the pool.
    void release(std::span<std::byte> buf) noexcept {
        auto offset = buf.data() - storage_.data();
        assert(offset >= 0);
        auto idx = static_cast<std::uint32_t>(static_cast<std::size_t>(offset) / buf_size_);
        free_list_.push_back(idx);
    }

    std::size_t buf_size() const noexcept { return buf_size_; }
    std::size_t available() const noexcept { return free_list_.size(); }

private:
    std::size_t buf_size_;
    std::vector<std::byte> storage_;
    std::vector<std::uint32_t> free_list_;
};

} // namespace bsomeip::io
