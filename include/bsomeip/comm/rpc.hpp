// SPDX-License-Identifier: MIT
// SOME/IP RPC: typed request-response pattern.
//
// Wraps proxy::async_call with a typed interface:
//   comm::rpc<add_request, add_response> add_call(prx, method_id);
//   auto resp = sync_wait(add_call(req));
//
// Also provides batch_rpc for pipelining multiple calls:
//   auto [r1, r2] = sync_wait(when_all(rpc1(req1), rpc2(req2)));
#pragma once

#include <cstddef>
#include <type_traits>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/proxy.hpp>

namespace bsomeip::comm {

// ============================================================
// Typed RPC call: wraps async_call with fixed Req/Resp types.
// ============================================================

template <typename Req, typename Resp>
    requires (std::is_aggregate_v<Req> && std::is_aggregate_v<Resp>)
class rpc {
public:
    rpc(api::proxy<>& prx, wire::method_id method)
        : prx_{prx}, method_{method} {}

    // Invoke the RPC. Returns a sender that completes with Resp.
    auto operator()(const Req& request) {
        return prx_.template async_call<Req, Resp>(method_, request);
    }

    wire::method_id method() const noexcept { return method_; }

private:
    api::proxy<>& prx_;
    wire::method_id method_;
};

} // namespace bsomeip::comm
