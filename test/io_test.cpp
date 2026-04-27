// SPDX-License-Identifier: MIT
// Tests for bsomeip::io — uring_context, framer, buffer_pool.
#include <bsomeip/io/uring_context.hpp>
#include <bsomeip/io/uring_scheduler.hpp>
#include <bsomeip/io/buffer_pool.hpp>
#include <bsomeip/io/framer.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/constants.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace bsomeip;

// ============================================================
// Test 1: uring_context creation and NOP submission
// ============================================================
void test_uring_context_nop() {
    io::uring_context ctx{32};
    assert(ctx.fd() >= 0);
    assert(ctx.sq_capacity() >= 32);

    // Submit a NOP
    auto* sqe = ctx.get_sqe();
    assert(sqe != nullptr);
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 42;

    int ret = ctx.submit();
    assert(ret >= 0);

    // Wait for completion
    ret = ctx.submit_and_wait(1);
    assert(ret >= 0);

    auto* cqe = ctx.peek_cqe();
    assert(cqe != nullptr);
    assert(cqe->user_data == 42);
    assert(cqe->res == 0); // NOP always succeeds
    ctx.seen_cqe();

    std::printf("[PASS] test_uring_context_nop\n");
}

// ============================================================
// Test 2: uring_context batch NOP
// ============================================================
void test_uring_batch_nop() {
    io::uring_context ctx{64};

    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        auto* sqe = ctx.get_sqe();
        assert(sqe != nullptr);
        sqe->opcode = IORING_OP_NOP;
        sqe->user_data = static_cast<std::uint64_t>(i);
    }

    ctx.submit_and_wait(N);

    unsigned count = ctx.for_each_cqe([](struct io_uring_cqe* cqe) {
        assert(cqe->res == 0);
    });
    assert(count == N);

    std::printf("[PASS] test_uring_batch_nop\n");
}

// ============================================================
// Test 3: uring_event_loop run_once with NOP callback
// ============================================================
void test_event_loop_run_once() {
    io::uring_event_loop loop{32};
    auto& ctx = loop.context();

    // Use a real callback via uring_op_base
    bool completed = false;
    struct test_op : io::detail::uring_op_base {
        bool* flag;
    };
    test_op op;
    op.flag = &completed;
    op.callback_ = [](io::detail::uring_op_base* base, int, unsigned) noexcept {
        auto* self = static_cast<test_op*>(base);
        *self->flag = true;
    };

    auto* sqe = ctx.get_sqe();
    assert(sqe != nullptr);
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = reinterpret_cast<std::uint64_t>(
        static_cast<io::detail::uring_op_base*>(&op));

    // Submit and wait for completion
    ctx.submit_and_wait(1);
    unsigned processed = loop.run_once();
    assert(completed);

    std::printf("[PASS] test_event_loop_run_once\n");
}

// ============================================================
// Test 4: buffer_pool acquire/release
// ============================================================
void test_buffer_pool() {
    io::buffer_pool pool{4, 1024};
    assert(pool.available() == 4);
    assert(pool.buf_size() == 1024);

    auto b1 = pool.acquire();
    assert(b1.size() == 1024);
    assert(pool.available() == 3);

    auto b2 = pool.acquire();
    auto b3 = pool.acquire();
    auto b4 = pool.acquire();
    assert(pool.available() == 0);

    auto b5 = pool.acquire();
    assert(b5.empty()); // Pool exhausted

    pool.release(b2);
    assert(pool.available() == 1);

    auto b6 = pool.acquire();
    assert(b6.size() == 1024);
    assert(pool.available() == 0);

    pool.release(b1);
    pool.release(b3);
    pool.release(b4);
    pool.release(b6);
    assert(pool.available() == 4);

    std::printf("[PASS] test_buffer_pool\n");
}

// ============================================================
// Test 5: framer — single complete message
// ============================================================
void test_framer_single_message() {
    io::framer framer;

    // Build a 16-byte SOME/IP message (header only, no payload)
    auto hdr = wire::header::make(
        wire::service_id{0x1234}, wire::method_id{0x5678},
        wire::client_id{0x0001}, wire::session_id{0x0001});

    framer.feed({reinterpret_cast<const std::byte*>(hdr.bytes), wire::header_size});

    auto msg = framer.next_message();
    assert(msg.size() == wire::header_size);

    // Verify header content
    wire::const_header_view v{msg.data()};
    assert(v.service() == wire::service_id{0x1234});
    assert(v.method() == wire::method_id{0x5678});

    // No more messages
    auto msg2 = framer.next_message();
    assert(msg2.empty());

    std::printf("[PASS] test_framer_single_message\n");
}

// ============================================================
// Test 6: framer — partial data then complete
// ============================================================
void test_framer_partial() {
    io::framer framer;

    // Build a message with 4 bytes of payload
    auto hdr = wire::header::make(
        wire::service_id{0x1234}, wire::method_id{0x5678},
        wire::client_id{0x0001}, wire::session_id{0x0001});
    auto v = hdr.view();
    v.set_payload_length(4);

    std::vector<std::byte> full_msg(wire::header_size + 4);
    std::memcpy(full_msg.data(), hdr.bytes, wire::header_size);
    full_msg[16] = std::byte{0xAA};
    full_msg[17] = std::byte{0xBB};
    full_msg[18] = std::byte{0xCC};
    full_msg[19] = std::byte{0xDD};

    // Feed first 10 bytes
    framer.feed({full_msg.data(), 10});
    auto msg = framer.next_message();
    assert(msg.empty()); // Not enough data

    // Feed remaining 10 bytes
    framer.feed({full_msg.data() + 10, 10});
    msg = framer.next_message();
    assert(msg.size() == 20);

    std::printf("[PASS] test_framer_partial\n");
}

// ============================================================
// Test 7: framer — two messages back to back
// ============================================================
void test_framer_multiple() {
    io::framer framer;

    auto hdr1 = wire::header::make(
        wire::service_id{0x0001}, wire::method_id{0x0001},
        wire::client_id{0x0001}, wire::session_id{0x0001});
    auto hdr2 = wire::header::make(
        wire::service_id{0x0002}, wire::method_id{0x0002},
        wire::client_id{0x0001}, wire::session_id{0x0002});

    // Feed both messages at once
    std::vector<std::byte> data(wire::header_size * 2);
    std::memcpy(data.data(), hdr1.bytes, wire::header_size);
    std::memcpy(data.data() + wire::header_size, hdr2.bytes, wire::header_size);

    framer.feed(data);

    auto msg1 = framer.next_message();
    assert(msg1.size() == wire::header_size);
    wire::const_header_view v1{msg1.data()};
    assert(v1.service() == wire::service_id{0x0001});

    auto msg2 = framer.next_message();
    assert(msg2.size() == wire::header_size);
    wire::const_header_view v2{msg2.data()};
    assert(v2.service() == wire::service_id{0x0002});

    auto msg3 = framer.next_message();
    assert(msg3.empty());

    std::printf("[PASS] test_framer_multiple\n");
}

// ============================================================
// Main
// ============================================================
int main() {
    test_uring_context_nop();
    test_uring_batch_nop();
    test_event_loop_run_once();
    test_buffer_pool();
    test_framer_single_message();
    test_framer_partial();
    test_framer_multiple();

    std::printf("\n=== All io tests passed ===\n");
    return 0;
}
