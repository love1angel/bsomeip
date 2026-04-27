// SPDX-License-Identifier: MIT
// bsomeip vs vsomeip architecture comparison benchmark.
//
// We implement vsomeip's actual serialization/deserialization approach
// (from their source code) inline — same algorithmic behavior:
//   - Serializer: push_back per byte into vector<byte_t>
//   - Deserializer: vector copy + iterator position + remaining counter
//   - Message: shared_ptr<payload>, virtual inheritance, runtime::get()
//   - Header: per-field serialize call through virtual interface
//   - Endian: runtime endian check per read/write (bithelper pattern)
//
// This isolates the architectural overhead (allocation, virtual dispatch,
// runtime endian check, per-byte push_back) from compiler/platform differences.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <stdexec/execution.hpp>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/route/dispatcher.hpp>
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/skeleton.hpp>
#include <bsomeip/api/proxy.hpp>

namespace wire = bsomeip::wire;
namespace api = bsomeip::api;
namespace route = bsomeip::route;

// ============================================================
// Timing
// ============================================================

using bench_clock = std::chrono::high_resolution_clock;

struct bench_result {
    const char* name;
    std::uint64_t iterations;
    double elapsed_ns;
    double ns_per_op() const { return elapsed_ns / iterations; }
    double ops_per_sec() const { return iterations / (elapsed_ns * 1e-9); }
};

void print_result(const bench_result& r) {
    std::printf("  %-45s  %10.1f ns/op  %12.0f ops/sec\n",
                r.name, r.ns_per_op(), r.ops_per_sec());
}

void print_comparison(const bench_result& bsomeip_r, const bench_result& vsomeip_r) {
    double speedup = vsomeip_r.ns_per_op() / bsomeip_r.ns_per_op();
    std::printf("  %-45s  %10.1f ns/op  vs  %10.1f ns/op  → %.1fx\n",
                bsomeip_r.name,
                bsomeip_r.ns_per_op(), vsomeip_r.ns_per_op(), speedup);
}

template <typename Fn>
bench_result benchmark(const char* name, std::uint64_t iters, Fn&& fn) {
    for (std::uint64_t i = 0; i < std::min(iters / 10, std::uint64_t{1000}); ++i)
        fn();
    auto start = bench_clock::now();
    for (std::uint64_t i = 0; i < iters; ++i)
        fn();
    auto end = bench_clock::now();
    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return {name, iters, ns};
}

static void do_not_optimize(void* p) {
    asm volatile("" : : "r,m"(p) : "memory");
}

// ============================================================
// vsomeip-equivalent serializer (from vsomeip source)
//
// Key behaviors replicated:
//   1. push_back per byte (no pre-allocation)
//   2. Runtime endian check per field via bithelper
//   3. reset() with shrink threshold
// ============================================================

namespace vsomeip_equiv {

using byte_t = std::uint8_t;

// Replicate vsomeip bithelper's runtime endian detection
inline bool is_little_endian() {
    static const std::uint16_t test = 1;
    return *reinterpret_cast<const std::uint8_t*>(&test) == 1;
}

inline std::uint16_t swap16(std::uint16_t v) {
    return (v >> 8) | (v << 8);
}

inline std::uint32_t swap32(std::uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

inline std::uint64_t swap64(std::uint64_t v) {
    return static_cast<std::uint64_t>(swap32(static_cast<std::uint32_t>(v))) << 32 |
           swap32(static_cast<std::uint32_t>(v >> 32));
}

// vsomeip serializer: push_back per byte into vector
class serializer {
public:
    serializer(std::uint32_t shrink_threshold = 0)
        : shrink_count_{0}, threshold_{shrink_threshold} {}

    bool serialize(std::uint8_t v) {
        data_.push_back(v);
        return true;
    }

    bool serialize(std::uint16_t v) {
        // vsomeip does: write_uint16_le, then push byte[1], byte[0] → big endian
        if (is_little_endian()) v = swap16(v);
        auto* p = reinterpret_cast<const byte_t*>(&v);
        data_.push_back(p[0]);
        data_.push_back(p[1]);
        return true;
    }

    bool serialize(std::uint32_t v, bool omit_last = false) {
        if (is_little_endian()) v = swap32(v);
        auto* p = reinterpret_cast<const byte_t*>(&v);
        if (!omit_last) data_.push_back(p[0]);
        data_.push_back(p[1]);
        data_.push_back(p[2]);
        data_.push_back(p[3]);
        return true;
    }

    bool serialize(std::int32_t v) {
        return serialize(static_cast<std::uint32_t>(v));
    }

    bool serialize(const byte_t* data, std::uint32_t len) {
        data_.insert(data_.end(), data, data + len);
        return true;
    }

    bool serialize(float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        return serialize(bits);
    }

    bool serialize(std::uint64_t v) {
        if (is_little_endian()) v = swap64(v);
        auto* p = reinterpret_cast<const byte_t*>(&v);
        for (int i = 0; i < 8; ++i) data_.push_back(p[i]);
        return true;
    }

    const byte_t* get_data() const { return data_.data(); }
    std::uint32_t get_size() const { return static_cast<std::uint32_t>(data_.size()); }

    void reset() {
        if (threshold_) {
            if (data_.size() < (data_.capacity() >> 1))
                shrink_count_++;
            else
                shrink_count_ = 0;
        }
        data_.clear();
        if (threshold_ && shrink_count_ > threshold_) {
            data_.shrink_to_fit();
            shrink_count_ = 0;
        }
    }

private:
    std::vector<byte_t> data_;
    std::uint32_t shrink_count_;
    std::uint32_t threshold_;
};

// vsomeip deserializer: vector copy + iterator
class deserializer {
public:
    deserializer(const byte_t* data, std::size_t len)
        : data_(data, data + len), position_(data_.begin()), remaining_(len) {}

    bool deserialize(std::uint8_t& v) {
        if (remaining_ == 0) return false;
        v = *position_++;
        remaining_--;
        return true;
    }

    bool deserialize(std::uint16_t& v) {
        if (remaining_ < 2) return false;
        byte_t b[2];
        b[0] = *position_++;
        b[1] = *position_++;
        remaining_ -= 2;
        std::memcpy(&v, b, 2);
        if (is_little_endian()) v = swap16(v);
        return true;
    }

    bool deserialize(std::uint32_t& v) {
        if (remaining_ < 4) return false;
        byte_t b[4];
        b[0] = *position_++;
        b[1] = *position_++;
        b[2] = *position_++;
        b[3] = *position_++;
        remaining_ -= 4;
        std::memcpy(&v, b, 4);
        if (is_little_endian()) v = swap32(v);
        return true;
    }

    bool deserialize(std::int32_t& v) {
        std::uint32_t u;
        if (!deserialize(u)) return false;
        v = static_cast<std::int32_t>(u);
        return true;
    }

    bool deserialize(float& v) {
        std::uint32_t bits;
        if (!deserialize(bits)) return false;
        std::memcpy(&v, &bits, 4);
        return true;
    }

    bool deserialize(std::uint64_t& v) {
        if (remaining_ < 8) return false;
        byte_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = *position_++;
        remaining_ -= 8;
        std::memcpy(&v, b, 8);
        if (is_little_endian()) v = swap64(v);
        return true;
    }

    std::size_t get_remaining() const { return remaining_; }

private:
    std::vector<byte_t> data_;   // vsomeip copies data into its own vector
    std::vector<byte_t>::iterator position_;
    std::size_t remaining_;
};

// vsomeip-style message: shared_ptr<payload>, virtual dispatch
struct payload {
    std::vector<byte_t> data_;
    void set_data(const byte_t* d, std::uint32_t len) {
        data_.assign(d, d + len);
    }
    std::uint32_t get_length() const {
        return static_cast<std::uint32_t>(data_.size());
    }
};

struct message {
    std::uint16_t service_id_{};
    std::uint16_t method_id_{};
    std::uint32_t length_{};
    std::uint16_t client_id_{};
    std::uint16_t session_id_{};
    std::uint8_t proto_ver_{1};
    std::uint8_t iface_ver_{};
    std::uint8_t msg_type_{};
    std::uint8_t ret_code_{};
    std::shared_ptr<payload> payload_;

    message() : payload_(std::make_shared<payload>()) {}

    // vsomeip message::serialize: header fields + payload
    bool serialize_to(serializer& s) const {
        s.serialize(service_id_);
        s.serialize(method_id_);
        s.serialize(length_);
        s.serialize(client_id_);
        s.serialize(session_id_);
        s.serialize(proto_ver_);
        s.serialize(iface_ver_);
        s.serialize(msg_type_);
        s.serialize(ret_code_);
        if (payload_ && payload_->get_length() > 0)
            s.serialize(payload_->data_.data(), payload_->get_length());
        return true;
    }

    // vsomeip message::deserialize: creates new payload via runtime
    bool deserialize_from(deserializer& d) {
        d.deserialize(service_id_);
        d.deserialize(method_id_);
        d.deserialize(length_);
        d.deserialize(client_id_);
        d.deserialize(session_id_);
        d.deserialize(proto_ver_);
        d.deserialize(iface_ver_);
        d.deserialize(msg_type_);
        d.deserialize(ret_code_);
        return true;
    }
};

// vsomeip-style dispatch: virtual handler via std::function + map
using handler_t = std::function<void(const std::shared_ptr<message>&)>;

class routing_manager {
public:
    void register_handler(std::uint32_t key, handler_t h) {
        handlers_[key] = std::move(h);
    }

    bool dispatch(std::uint32_t key, const std::shared_ptr<message>& msg) {
        auto it = handlers_.find(key);
        if (it != handlers_.end()) {
            it->second(msg);
            return true;
        }
        return false;
    }

private:
    std::unordered_map<std::uint32_t, handler_t> handlers_;
};

} // namespace vsomeip_equiv

// ============================================================
// Test payloads
// ============================================================

struct small_payload { std::int32_t a; std::int32_t b; };

struct medium_payload {
    std::uint32_t id;
    std::int32_t x; std::int32_t y; std::int32_t z;
    float temperature;
    float pressure;
    float humidity;
    std::uint64_t timestamp;
    std::uint32_t flags;
    std::int32_t quality;
    std::int32_t reserved1;
    std::uint32_t sequence;
    std::uint32_t checksum;
};

// ============================================================
// Benchmark 1: Header serialize/deserialize
// ============================================================

void bench_header_comparison() {
    std::printf("\n=== Header Write/Read ===\n");
    constexpr std::uint64_t N = 5'000'000;

    // bsomeip: zero-copy header_view
    std::array<std::byte, 16> buf{};
    wire::header_view hdr{buf.data()};

    auto b1 = benchmark("[bsomeip] header write", N, [&] {
        hdr.set_service(wire::service_id{0x1234});
        hdr.set_method(wire::method_id{0x0001});
        hdr.set_payload_length(100);
        hdr.set_client(wire::client_id{0x0010});
        hdr.set_session(wire::session_id{42});
        hdr.set_protocol_ver(1);
        hdr.set_interface_ver(1);
        hdr.set_msg_type(wire::message_type::request);
        hdr.set_ret_code(wire::return_code::e_ok);
        do_not_optimize(buf.data());
    });

    // vsomeip-equiv: serializer push_back
    vsomeip_equiv::serializer ser;
    auto v1 = benchmark("[vsomeip] header write (serializer)", N, [&] {
        ser.reset();
        ser.serialize(std::uint16_t{0x1234}); // service
        ser.serialize(std::uint16_t{0x0001}); // method
        ser.serialize(std::uint32_t{108});     // length (8 + payload)
        ser.serialize(std::uint16_t{0x0010}); // client
        ser.serialize(std::uint16_t{42});     // session
        ser.serialize(std::uint8_t{1});       // proto_ver
        ser.serialize(std::uint8_t{1});       // iface_ver
        ser.serialize(std::uint8_t{0x00});    // msg_type = request
        ser.serialize(std::uint8_t{0x00});    // ret_code = ok
        do_not_optimize(const_cast<vsomeip_equiv::byte_t*>(ser.get_data()));
    });

    print_comparison(b1, v1);

    // Read
    auto b2 = benchmark("[bsomeip] header read", N, [&] {
        auto s = hdr.service();
        auto m = hdr.method();
        auto c = hdr.client();
        auto ss = hdr.session();
        auto mt = hdr.msg_type();
        auto rc = hdr.ret_code();
        auto pl = hdr.payload_length();
        do_not_optimize(&s);
        do_not_optimize(&pl);
    });

    // vsomeip-equiv: deserializer per-field
    wire::header_view src{buf.data()};
    src.set_service(wire::service_id{0x1234});
    src.set_method(wire::method_id{0x0001});
    src.set_payload_length(100);
    src.set_client(wire::client_id{0x0010});
    src.set_session(wire::session_id{42});

    auto v2 = benchmark("[vsomeip] header read (deserializer)", N, [&] {
        vsomeip_equiv::deserializer deser(
            reinterpret_cast<const vsomeip_equiv::byte_t*>(buf.data()), 16);
        std::uint16_t svc, mth, cli, ses;
        std::uint32_t len;
        std::uint8_t pv, iv, mt2, rc2;
        deser.deserialize(svc);
        deser.deserialize(mth);
        deser.deserialize(len);
        deser.deserialize(cli);
        deser.deserialize(ses);
        deser.deserialize(pv);
        deser.deserialize(iv);
        deser.deserialize(mt2);
        deser.deserialize(rc2);
        do_not_optimize(&svc);
        do_not_optimize(&rc2);
    });

    print_comparison(b2, v2);
}

// ============================================================
// Benchmark 2: Payload serialize (small + medium)
// ============================================================

void bench_serialize_comparison() {
    std::printf("\n=== Payload Serialize ===\n");
    constexpr std::uint64_t N = 1'000'000;

    // Small (8 bytes)
    {
        small_payload src{42, -100};
        std::array<std::byte, 64> buf{};

        auto b = benchmark("[bsomeip] serialize small (8B)", N, [&] {
            wire::serialize(std::span{buf}, src);
            do_not_optimize(buf.data());
        });

        vsomeip_equiv::serializer ser;
        auto v = benchmark("[vsomeip] serialize small (8B)", N, [&] {
            ser.reset();
            ser.serialize(src.a);
            ser.serialize(src.b);
            do_not_optimize(const_cast<vsomeip_equiv::byte_t*>(ser.get_data()));
        });

        print_comparison(b, v);
    }

    // Medium (52 bytes)
    {
        medium_payload src{1, 100, 200, 300, 36.5f, 1013.25f, 65.0f,
                           1234567890ULL, 0xFF, 90, 0, 42, 0xDEAD};
        std::array<std::byte, 256> buf{};

        auto b = benchmark("[bsomeip] serialize medium (52B)", N, [&] {
            wire::serialize(std::span{buf}, src);
            do_not_optimize(buf.data());
        });

        vsomeip_equiv::serializer ser;
        auto v = benchmark("[vsomeip] serialize medium (52B)", N, [&] {
            ser.reset();
            ser.serialize(src.id);
            ser.serialize(src.x);
            ser.serialize(src.y);
            ser.serialize(src.z);
            ser.serialize(src.temperature);
            ser.serialize(src.pressure);
            ser.serialize(src.humidity);
            ser.serialize(src.timestamp);
            ser.serialize(src.flags);
            ser.serialize(src.quality);
            ser.serialize(src.reserved1);
            ser.serialize(src.sequence);
            ser.serialize(src.checksum);
            do_not_optimize(const_cast<vsomeip_equiv::byte_t*>(ser.get_data()));
        });

        print_comparison(b, v);
    }
}

// ============================================================
// Benchmark 3: Payload deserialize (small + medium)
// ============================================================

void bench_deserialize_comparison() {
    std::printf("\n=== Payload Deserialize ===\n");
    constexpr std::uint64_t N = 1'000'000;

    // Small
    {
        small_payload src{42, -100};
        std::array<std::byte, 64> buf{};
        wire::serialize(std::span{buf}, src);

        auto b = benchmark("[bsomeip] deserialize small (8B)", N, [&] {
            auto r = wire::deserialize<small_payload>(
                std::span<const std::byte>{buf.data(), 8});
            do_not_optimize(&r);
        });

        auto v = benchmark("[vsomeip] deserialize small (8B)", N, [&] {
            vsomeip_equiv::deserializer d(
                reinterpret_cast<const vsomeip_equiv::byte_t*>(buf.data()), 8);
            std::int32_t a, b2;
            d.deserialize(a);
            d.deserialize(b2);
            do_not_optimize(&a);
        });

        print_comparison(b, v);
    }

    // Medium
    {
        medium_payload src{1, 100, 200, 300, 36.5f, 1013.25f, 65.0f,
                           1234567890ULL, 0xFF, 90, 0, 42, 0xDEAD};
        std::array<std::byte, 256> buf{};
        wire::serialize(std::span{buf}, src);

        auto b = benchmark("[bsomeip] deserialize medium (52B)", N, [&] {
            auto r = wire::deserialize<medium_payload>(
                std::span<const std::byte>{buf.data(), 52});
            do_not_optimize(&r);
        });

        auto v = benchmark("[vsomeip] deserialize medium (52B)", N, [&] {
            vsomeip_equiv::deserializer d(
                reinterpret_cast<const vsomeip_equiv::byte_t*>(buf.data()), 52);
            medium_payload out{};
            d.deserialize(out.id);
            d.deserialize(out.x);
            d.deserialize(out.y);
            d.deserialize(out.z);
            d.deserialize(out.temperature);
            d.deserialize(out.pressure);
            d.deserialize(out.humidity);
            d.deserialize(out.timestamp);
            d.deserialize(out.flags);
            d.deserialize(out.quality);
            d.deserialize(out.reserved1);
            d.deserialize(out.sequence);
            d.deserialize(out.checksum);
            do_not_optimize(&out);
        });

        print_comparison(b, v);
    }
}

// ============================================================
// Benchmark 4: Message create + serialize (full header+payload)
// ============================================================

void bench_message_comparison() {
    std::printf("\n=== Full Message Create + Serialize ===\n");
    constexpr std::uint64_t N = 500'000;

    small_payload payload_data{42, -100};

    // bsomeip: stack-based message::create_request + serialize into it
    auto b = benchmark("[bsomeip] create+serialize msg (8B)", N, [&] {
        auto msg = api::message::create_request(
            wire::service_id{0x1234}, wire::method_id{0x0001},
            wire::client_id{0x0010}, wire::session_id{1}, 8);
        wire::serialize(msg.payload(), payload_data);
        do_not_optimize(msg.data.data());
    });

    // vsomeip-equiv: shared_ptr<message> + serializer
    auto v = benchmark("[vsomeip] create+serialize msg (8B)", N, [&] {
        auto msg = std::make_shared<vsomeip_equiv::message>();
        msg->service_id_ = 0x1234;
        msg->method_id_ = 0x0001;
        msg->client_id_ = 0x0010;
        msg->session_id_ = 1;
        msg->msg_type_ = 0x00;
        msg->length_ = 16; // 8 header + 8 payload

        // Serialize payload into a buffer, then set on message
        vsomeip_equiv::serializer ser;
        ser.serialize(payload_data.a);
        ser.serialize(payload_data.b);
        msg->payload_->set_data(ser.get_data(), ser.get_size());

        // Now serialize the whole message (like vsomeip does before sending)
        vsomeip_equiv::serializer msg_ser;
        msg->serialize_to(msg_ser);
        do_not_optimize(const_cast<vsomeip_equiv::byte_t*>(msg_ser.get_data()));
    });

    print_comparison(b, v);
}

// ============================================================
// Benchmark 5: Message dispatch
// ============================================================

void bench_dispatch_comparison() {
    std::printf("\n=== Message Dispatch ===\n");
    constexpr std::uint64_t N = 1'000'000;

    // bsomeip: dispatcher with method_key hash
    route::dispatcher bdisp;
    volatile int b_count = 0;
    bdisp.register_handler(wire::service_id{0x1234}, wire::method_id{0x0001},
        [&](const route::message_view&) { b_count++; });

    std::array<std::byte, 24> msg_buf{};
    wire::header_view hdr{msg_buf.data()};
    hdr.set_service(wire::service_id{0x1234});
    hdr.set_method(wire::method_id{0x0001});
    hdr.set_msg_type(wire::message_type::request);
    hdr.set_payload_length(8);
    auto payload = std::span{msg_buf}.subspan(wire::header_size);

    auto b = benchmark("[bsomeip] dispatch", N, [&] {
        bdisp.dispatch(hdr, payload);
    });

    // vsomeip-equiv: shared_ptr<message> + function<void(shared_ptr<message>)>
    vsomeip_equiv::routing_manager vrm;
    volatile int v_count = 0;
    std::uint32_t key = (0x1234u << 16) | 0x0001u;
    vrm.register_handler(key, [&](const std::shared_ptr<vsomeip_equiv::message>&) {
        v_count++;
    });

    auto vmsg = std::make_shared<vsomeip_equiv::message>();
    vmsg->service_id_ = 0x1234;
    vmsg->method_id_ = 0x0001;

    auto v = benchmark("[vsomeip] dispatch (shared_ptr msg)", N, [&] {
        vrm.dispatch(key, vmsg);
    });

    print_comparison(b, v);
}

// ============================================================
// Benchmark 6: Full round-trip (serialize → dispatch → deserialize)
// ============================================================

void bench_roundtrip_comparison() {
    std::printf("\n=== Full Request→Response Round-Trip ===\n");
    constexpr std::uint64_t N = 500'000;

    // bsomeip: async_call (sender-based)
    {
        api::application app;
        struct svc_impl {
            small_payload handle(const small_payload& r) { return {r.a + r.b, 0}; }
        };
        svc_impl impl;
        api::skeleton<svc_impl> skel(app, impl);
        skel.offer(wire::service_id{0x1234}, wire::instance_id{0x0001}, 1, 0);
        skel.serve<small_payload, small_payload>(
            wire::method_id{0x0001},
            [](svc_impl& s, const small_payload& r) { return s.handle(r); });
        api::proxy<> prx(app);
        prx.target(wire::service_id{0x1234}, wire::instance_id{0x0001});

        small_payload req{10, 20};
        auto b = benchmark("[bsomeip] full round-trip (sender)", N, [&] {
            auto result = stdexec::sync_wait(
                prx.async_call<small_payload, small_payload>(
                    wire::method_id{0x0001}, req));
            do_not_optimize(&result);
        });
        print_result(b);
    }

    // vsomeip-equiv: manual serialize → dispatch → deserialize
    {
        vsomeip_equiv::routing_manager rm;

        // Register handler that deserializes request, computes, serializes response
        std::vector<vsomeip_equiv::byte_t> response_buf;

        rm.register_handler(
            (0x1234u << 16) | 0x0001u,
            [&](const std::shared_ptr<vsomeip_equiv::message>& req) {
                // Deserialize request payload
                vsomeip_equiv::deserializer d(
                    req->payload_->data_.data(), req->payload_->get_length());
                std::int32_t a, b;
                d.deserialize(a);
                d.deserialize(b);

                // Compute
                std::int32_t result = a + b;

                // Serialize response
                vsomeip_equiv::serializer ser;
                ser.serialize(result);
                ser.serialize(std::int32_t{0}); // second field
                response_buf.assign(ser.get_data(), ser.get_data() + ser.get_size());
            });

        auto v = benchmark("[vsomeip] full round-trip (callback)", N, [&] {
            // 1. Create request message (shared_ptr + payload)
            auto msg = std::make_shared<vsomeip_equiv::message>();
            msg->service_id_ = 0x1234;
            msg->method_id_ = 0x0001;
            msg->client_id_ = 0x0010;
            msg->session_id_ = 1;

            // 2. Serialize payload
            vsomeip_equiv::serializer ser;
            ser.serialize(std::int32_t{10});
            ser.serialize(std::int32_t{20});
            msg->payload_->set_data(ser.get_data(), ser.get_size());

            // 3. Dispatch (handler runs inline for in-process)
            rm.dispatch((0x1234u << 16) | 0x0001u, msg);

            // 4. Deserialize response
            vsomeip_equiv::deserializer d(response_buf.data(), response_buf.size());
            std::int32_t result, pad;
            d.deserialize(result);
            d.deserialize(pad);
            do_not_optimize(&result);
        });
        print_result(v);
    }
}

// ============================================================
// Summary
// ============================================================

int main() {
    std::printf("=== bsomeip vs vsomeip Architecture Comparison ===\n");
    std::printf("vsomeip logic faithfully replicated from source:\n");
    std::printf("  - serializer: push_back per byte, runtime endian check\n");
    std::printf("  - deserializer: vector copy + iterator, per-field bounds check\n");
    std::printf("  - message: shared_ptr<payload>, virtual dispatch\n");
    std::printf("  - dispatch: std::function<void(shared_ptr<message>)>\n");
    std::printf("\nbsomeip approach:\n");
    std::printf("  - codec: reflection-generated, direct write to span<byte>\n");
    std::printf("  - header: zero-copy header_view over existing buffer\n");
    std::printf("  - message: value type (vector<byte>), no heap for header\n");
    std::printf("  - dispatch: stdexec senders, no shared_ptr per message\n");
    std::printf("\nFormat: [bsomeip] ns/op  vs  [vsomeip] ns/op  →  speedup\n");

    bench_header_comparison();
    bench_serialize_comparison();
    bench_deserialize_comparison();
    bench_message_comparison();
    bench_dispatch_comparison();
    bench_roundtrip_comparison();

    std::printf("\n=== Comparison complete ===\n");
    return 0;
}
