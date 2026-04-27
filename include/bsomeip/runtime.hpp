// SPDX-License-Identifier: MIT
// bsomeip runtime: application lifecycle, async sender adaptors,
// communication patterns, and platform-specific I/O backends.
//
// Depends on: bsomeip/core.hpp + bsomeip/async/execution.hpp
// Platform I/O is conditionally included based on platform.
#pragma once

// Core protocol engine
#include <bsomeip/core.hpp>

// Async execution abstraction (isolates stdexec/P2300)
#include <bsomeip/async/execution.hpp>

// Application, proxy, skeleton
#include <bsomeip/api/application.hpp>
#include <bsomeip/api/proxy.hpp>
#include <bsomeip/api/skeleton.hpp>

// Sender adaptors: E2E protect/check, security enforce
#include <bsomeip/e2e/protector.hpp>
#include <bsomeip/security/enforcer.hpp>

// Communication patterns: attribute, broadcast, RPC
#include <bsomeip/comm/attribute.hpp>
#include <bsomeip/comm/broadcast.hpp>
#include <bsomeip/comm/rpc.hpp>

// Platform-specific I/O backend
#if defined(__linux__)
#include <bsomeip/io/uring_context.hpp>
#include <bsomeip/io/uring_scheduler.hpp>
#include <bsomeip/io/socket_ops.hpp>
#endif

// I/O utilities (platform-neutral)
#include <bsomeip/io/buffer_pool.hpp>
#include <bsomeip/io/framer.hpp>

// vsomeip compatibility shim
#include <bsomeip/compat/vsomeip.hpp>
