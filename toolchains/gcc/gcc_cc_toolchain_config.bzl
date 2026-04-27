"""Custom GCC toolchain configuration for Bazel.

Points to GCC built from third_party/compiler/gcc and installed
to tools/gcc_install/. Falls back to system GCC if not yet built.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)

def _impl(ctx):
    tool_paths = [
        tool_path(name = "gcc", path = "wrappers/gcc"),
        tool_path(name = "g++", path = "wrappers/g++"),
        tool_path(name = "ld", path = "wrappers/ld"),
        tool_path(name = "ar", path = "wrappers/ar"),
        tool_path(name = "nm", path = "wrappers/nm"),
        tool_path(name = "objdump", path = "wrappers/objdump"),
        tool_path(name = "strip", path = "wrappers/strip"),
        tool_path(name = "cpp", path = "wrappers/cpp"),
        tool_path(name = "gcov", path = "wrappers/gcov"),
    ]

    all_compile_actions = [
        ACTION_NAMES.c_compile,
        ACTION_NAMES.cpp_compile,
        ACTION_NAMES.cpp_header_parsing,
        ACTION_NAMES.cpp_module_compile,
        ACTION_NAMES.cpp_module_codegen,
        ACTION_NAMES.assemble,
        ACTION_NAMES.preprocess_assemble,
        ACTION_NAMES.linkstamp_compile,
    ]

    all_link_actions = [
        ACTION_NAMES.cpp_link_executable,
        ACTION_NAMES.cpp_link_dynamic_library,
        ACTION_NAMES.cpp_link_nodeps_dynamic_library,
    ]

    # Default compile flags
    default_compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-no-canonical-prefixes",
                            "-fno-canonical-system-headers",
                            "-Wno-builtin-macro-redefined",
                            "-D__DATE__=\"redacted\"",
                            "-D__TIMESTAMP__=\"redacted\"",
                            "-D__TIME__=\"redacted\"",
                        ],
                    ),
                ],
            ),
        ],
    )

    # C++26 with reflection (default for all C++ compilation)
    cpp26_feature = feature(
        name = "cpp26",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_compile, ACTION_NAMES.cpp_header_parsing],
                flag_groups = [
                    flag_group(flags = ["-std=c++26", "-fcoroutines", "-freflection"]),
                ],
            ),
        ],
    )

    # Link flags
    default_link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-lstdc++",
                            "-lm",
                            "-lpthread",
                            "-Wl,-rpath,/home/xp/self/github/bsomeip/tools/gcc_install/lib64",
                        ],
                    ),
                ],
            ),
        ],
    )

    # Unfiltered compile flags
    unfiltered_compile_flags = feature(
        name = "unfiltered_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fno-canonical-system-headers",
                            "-Wno-builtin-macro-redefined",
                        ],
                    ),
                ],
            ),
        ],
    )

    # Support for user-specified compile flags
    user_compile_flags = feature(
        name = "user_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "user_compile_flags",
                        flags = ["%{user_compile_flags}"],
                        iterate_over = "user_compile_flags",
                    ),
                ],
            ),
        ],
    )

    features = [
        default_compile_flags,
        cpp26_feature,
        default_link_flags,
        unfiltered_compile_flags,
        user_compile_flags,
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = features,
        toolchain_identifier = "gcc-trunk-toolchain",
        host_system_name = "x86_64-unknown-linux-gnu",
        target_system_name = "x86_64-unknown-linux-gnu",
        target_cpu = "x86_64",
        target_libc = "glibc",
        compiler = "gcc",
        abi_version = "gcc",
        abi_libc_version = "glibc",
        tool_paths = tool_paths,
        cxx_builtin_include_directories = [
            # GCC 16 (built from source) — absolute paths required for Bazel sandbox
            "/home/xp/self/github/bsomeip/tools/gcc_install/include/c++/16.0.1",
            "/home/xp/self/github/bsomeip/tools/gcc_install/include/c++/16.0.1/x86_64-pc-linux-gnu",
            "/home/xp/self/github/bsomeip/tools/gcc_install/include/c++/16.0.1/backward",
            "/home/xp/self/github/bsomeip/tools/gcc_install/lib/gcc/x86_64-pc-linux-gnu/16.0.1/include",
            "/home/xp/self/github/bsomeip/tools/gcc_install/lib/gcc/x86_64-pc-linux-gnu/16.0.1/include-fixed",
            "/home/xp/self/github/bsomeip/tools/gcc_install/include",
            # System headers
            "/usr/local/include",
            "/usr/include/x86_64-linux-gnu",
            "/usr/include",
        ],
    )

gcc_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
