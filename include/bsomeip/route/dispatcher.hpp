// SPDX-License-Identifier: MIT
// Message dispatcher: routes incoming SOME/IP messages to registered handlers
// by (service_id, method_id) for requests and (service_id, eventgroup_id) for events.
//
// Design: lock-free dispatch path. Handlers are registered at setup time
// (before the event loop starts). Dispatch is a simple hash lookup + function
// pointer call with zero allocation and zero synchronization.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <flat_map>
#include <functional>
#include <span>
#include <vector>

#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/message_type.hpp>

namespace bsomeip::route {

// A message view passed to handlers — non-owning reference to header + payload.
struct message_view {
    const wire::header_view& header;
    std::span<const std::byte> payload;
};

// ============================================================
// inplace_handler: type-erased callable with inline storage.
// No heap allocation. Fits lambdas with captures up to Capacity bytes.
// ============================================================
template <typename Sig, std::size_t Capacity = 48>
class inplace_handler;

template <typename R, typename... Args, std::size_t Capacity>
class inplace_handler<R(Args...), Capacity> {
    using invoke_fn = R(*)(void*, Args...);
    using destroy_fn = void(*)(void*);

    alignas(std::max_align_t) std::byte storage_[Capacity]{};
    invoke_fn invoke_{nullptr};
    destroy_fn destroy_{nullptr};

    template <typename F>
    static R invoke_impl(void* p, Args... args) {
        return (*static_cast<F*>(p))(static_cast<Args>(args)...);
    }

    template <typename F>
    static void destroy_impl(void* p) {
        static_cast<F*>(p)->~F();
    }

public:
    inplace_handler() = default;
    ~inplace_handler() { if (destroy_) destroy_(storage_); }

    inplace_handler(const inplace_handler&) = delete;
    inplace_handler& operator=(const inplace_handler&) = delete;

    inplace_handler(inplace_handler&& o) noexcept {
        std::memcpy(storage_, o.storage_, Capacity);
        invoke_ = o.invoke_;
        destroy_ = o.destroy_;
        o.invoke_ = nullptr;
        o.destroy_ = nullptr;
    }

    inplace_handler& operator=(inplace_handler&& o) noexcept {
        if (this != &o) {
            if (destroy_) destroy_(storage_);
            std::memcpy(storage_, o.storage_, Capacity);
            invoke_ = o.invoke_;
            destroy_ = o.destroy_;
            o.invoke_ = nullptr;
            o.destroy_ = nullptr;
        }
        return *this;
    }

    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, inplace_handler>
                  && sizeof(std::decay_t<F>) <= Capacity)
    inplace_handler(F&& f) {
        using Decayed = std::decay_t<F>;
        static_assert(sizeof(Decayed) <= Capacity,
                      "callable too large for inplace_handler");
        new (storage_) Decayed(std::forward<F>(f));
        invoke_ = &invoke_impl<Decayed>;
        destroy_ = &destroy_impl<Decayed>;
    }

    explicit operator bool() const noexcept { return invoke_ != nullptr; }

    R operator()(Args... args) const {
        return invoke_(const_cast<std::byte*>(storage_),
                       static_cast<Args>(args)...);
    }
};

// Handler types: inplace, no heap allocation.
using message_handler_t = inplace_handler<void(const message_view&)>;

// Availability handlers are rare (setup-only), keep std::function for flexibility.
using availability_handler_t = std::function<void(
    wire::service_id, wire::instance_id, bool /*available*/)>;

// Pack (service_id, method_id) into a single uint32_t for fast hash lookup.
constexpr std::uint32_t pack_key(wire::service_id s, wire::method_id m) noexcept {
    return (static_cast<std::uint32_t>(s.value) << 16) | m.value;
}

constexpr std::uint32_t pack_eg_key(wire::service_id s, wire::eventgroup_id e) noexcept {
    return (static_cast<std::uint32_t>(s.value) << 16) | e.value;
}

// Message dispatcher.
// Lock-free dispatch: register handlers at setup, dispatch without locks.
// Handlers are stored inline (no heap allocation).
class dispatcher {
public:
    // Register a handler for a specific (service, method).
    void register_handler(wire::service_id service, wire::method_id method,
                          message_handler_t handler) {
        handlers_[pack_key(service, method)] = std::move(handler);
    }

    // Remove a handler.
    void unregister_handler(wire::service_id service,
                            wire::method_id method) {
        handlers_.erase(pack_key(service, method));
    }

    // Subscribe to an eventgroup with a handler.
    void subscribe(wire::service_id service, wire::eventgroup_id eg,
                   message_handler_t handler) {
        subscriptions_[pack_eg_key(service, eg)].push_back(std::move(handler));
    }

    // Remove all subscriptions for an eventgroup.
    void unsubscribe(wire::service_id service, wire::eventgroup_id eg) {
        subscriptions_.erase(pack_eg_key(service, eg));
    }

    // Register an availability handler.
    void on_availability(wire::service_id service,
                         availability_handler_t handler) {
        availability_handlers_[service].push_back(std::move(handler));
    }

    // Dispatch a message to the appropriate handler.
    // Returns true if a handler was found and invoked.
    // LOCK-FREE: no mutex, no allocation. Pure hash lookup + function pointer call.
    bool dispatch(const wire::header_view& hdr,
                  std::span<const std::byte> payload) const {
        message_view view{hdr, payload};

        auto key = pack_key(hdr.service(), hdr.method());
        auto it = handlers_.find(key);
        if (it != handlers_.end()) {
            it->second(view);
            return true;
        }
        // Try wildcard method
        auto wk = pack_key(hdr.service(), wire::any_method);
        it = handlers_.find(wk);
        if (it != handlers_.end()) {
            it->second(view);
            return true;
        }
        return false;
    }

    // Dispatch a notification to eventgroup subscribers.
    void notify_subscribers(wire::service_id service,
                            wire::eventgroup_id eg,
                            const wire::header_view& hdr,
                            std::span<const std::byte> payload) const {
        message_view view{hdr, payload};
        auto it = subscriptions_.find(pack_eg_key(service, eg));
        if (it != subscriptions_.end()) {
            for (const auto& handler : it->second) {
                handler(view);
            }
        }
    }

    // Notify availability change to registered listeners.
    void notify_availability(wire::service_id service,
                             wire::instance_id instance,
                             bool available) const {
        auto it = availability_handlers_.find(service);
        if (it != availability_handlers_.end()) {
            for (const auto& handler : it->second) {
                handler(service, instance, available);
            }
        }
    }

    void clear() {
        handlers_.clear();
        subscriptions_.clear();
        availability_handlers_.clear();
    }

private:
    std::flat_map<std::uint32_t, message_handler_t> handlers_;

    std::flat_map<std::uint32_t,
        std::vector<message_handler_t>> subscriptions_;

    std::flat_map<wire::service_id,
        std::vector<availability_handler_t>> availability_handlers_;
};

} // namespace bsomeip::route
