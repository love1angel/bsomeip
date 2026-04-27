// SPDX-License-Identifier: MIT
// Async execution abstraction: isolates the third-party sender/receiver
// library (currently stdexec/P2300) behind bsomeip-owned type aliases.
//
// All bsomeip headers use bsomeip::async:: instead of stdexec:: directly.
// To switch to a different S/R implementation (e.g. std::execution in C++26),
// only this file needs to change.
#pragma once

#include <stdexec/execution.hpp>

namespace bsomeip::async {

// --- Concept tags ---
using sender_tag          = stdexec::sender_tag;
using receiver_tag        = stdexec::receiver_tag;
using operation_state_tag = stdexec::operation_state_tag;
using scheduler_tag       = stdexec::scheduler_tag;

// --- Completion signatures ---
template <typename... Sigs>
using completion_signatures = stdexec::completion_signatures<Sigs...>;

using set_value_t  = stdexec::set_value_t;
using set_error_t  = stdexec::set_error_t;
using set_stopped_t = stdexec::set_stopped_t;

// --- Completion functions ---
using stdexec::set_value;
using stdexec::set_error;
using stdexec::set_stopped;

// --- Sender/receiver operations ---
using stdexec::connect;
using stdexec::start;

// --- Sender factories & adaptors ---
using stdexec::just;
using stdexec::then;
using stdexec::let_value;
using stdexec::sync_wait;

// --- Environment ---
using stdexec::empty_env;
using stdexec::get_scheduler_t;

} // namespace bsomeip::async
