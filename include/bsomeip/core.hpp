// SPDX-License-Identifier: MIT
// bsomeip core: platform-independent SOME/IP protocol engine.
//
// Includes: wire format, codec, E2E profiles, CRC, security policy,
// service discovery protocol, message dispatch, service registry.
//
// No third-party dependencies. No platform-specific code.
// Safe to use on any platform with a C++26 compiler.
#pragma once

// Wire format: types, header, codec, message types, constants, TP
#include <bsomeip/wire/types.hpp>
#include <bsomeip/wire/header.hpp>
#include <bsomeip/wire/codec.hpp>
#include <bsomeip/wire/message_type.hpp>
#include <bsomeip/wire/return_code.hpp>
#include <bsomeip/wire/constants.hpp>
#include <bsomeip/wire/tp.hpp>

// E2E: CRC computation, AUTOSAR profiles
#include <bsomeip/e2e/crc.hpp>
#include <bsomeip/e2e/profile.hpp>

// Security: access control policy engine
#include <bsomeip/security/policy.hpp>

// Service Discovery: entries, options, message codec, state machine
#include <bsomeip/sd/entry.hpp>
#include <bsomeip/sd/option.hpp>
#include <bsomeip/sd/message.hpp>
#include <bsomeip/sd/discovery.hpp>

// Routing: dispatcher, registry, manager
#include <bsomeip/route/dispatcher.hpp>
#include <bsomeip/route/registry.hpp>
#include <bsomeip/route/manager.hpp>

// Configuration
#include <bsomeip/config/config.hpp>
