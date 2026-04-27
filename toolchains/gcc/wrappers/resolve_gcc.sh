#!/usr/bin/env bash
# Common setup for GCC 16 toolchain wrappers.
# Resolves GCC_INSTALL using both sandbox-safe and direct methods.

# Method 1: BSOMEIP_GCC_INSTALL env var (user override)
if [[ -n "$BSOMEIP_GCC_INSTALL" && -d "$BSOMEIP_GCC_INSTALL" ]]; then
    GCC_INSTALL="$BSOMEIP_GCC_INSTALL"
    return 0 2>/dev/null || true
fi

# Method 2: Bazel execution root — tools/gcc_install is a sibling of toolchains/
# In Bazel sandbox, $0 is something like:
#   .../execroot/bsomeip/toolchains/gcc/wrappers/gcc
# We need:
#   .../execroot/bsomeip/tools/gcc_install
SELF="$(readlink -f "$0" 2>/dev/null || realpath "$0" 2>/dev/null || echo "$0")"
WRAPPER_DIR="$(dirname "$SELF")"
# Go up from toolchains/gcc/wrappers → project root
PROJECT_ROOT="$(cd "$WRAPPER_DIR/../../.." 2>/dev/null && pwd)"

if [[ -x "$PROJECT_ROOT/tools/gcc_install/bin/gcc" ]]; then
    GCC_INSTALL="$PROJECT_ROOT/tools/gcc_install"
    return 0 2>/dev/null || true
fi

# Method 3: Absolute fallback for this workspace
GCC_INSTALL="/home/xp/self/github/bsomeip/tools/gcc_install"
if [[ -x "$GCC_INSTALL/bin/gcc" ]]; then
    return 0 2>/dev/null || true
fi

echo "ERROR: GCC 16 not found. Run: bash tools/build_gcc.sh" >&2
exit 1
