import os
import platform as host

import build


build.cxxflags += [
    "-std=c++23",
    "-O2",
    "-W",
    "-Wall",
    "-Werror",
]

libstd = dependency(
    ldflags=[]
    if "-Dno_vendored_std" in build.cppflags or "-lstd" in build.ldflags
    else ["-lstd"]
)

common_sources = [
    "$(S)/clipboard.cpp",
    "$(S)/drop.cpp",
    "$(S)/fiber.cpp",
    "$(S)/input.cpp",
    "$(S)/loop_wake.cpp",
    "$(S)/mutex.cpp",
    "$(S)/poller_loop.cpp",
    "$(S)/pointer_grab.cpp",
    "$(S)/platform.cpp",
    "$(S)/platform_headless.cpp",
    "$(S)/window.cpp",
]
target_platform = build.target
if "apple-darwin" in target_platform:
    system = "Darwin"
elif build.target != build.host:
    raise RuntimeError(f"unsupported target: {target_platform}")
else:
    system = host.system()

if system == "Linux":
    protocol_root = pkg_config_variable("wayland-protocols", "pkgdatadir")
    protocol_paths = [
        "stable/viewporter/viewporter",
        "stable/xdg-shell/xdg-shell",
        "staging/fractional-scale/fractional-scale-v1",
        "unstable/xdg-decoration/xdg-decoration-unstable-v1",
        "staging/xdg-activation/xdg-activation-v1",
        "unstable/primary-selection/primary-selection-unstable-v1",
        "unstable/tablet/tablet-unstable-v2",
        "unstable/text-input/text-input-unstable-v3",
        "staging/cursor-shape/cursor-shape-v1",
    ]
    protocol_outputs = []
    server_protocol_outputs = []
    protocol_commands = []
    for path in protocol_paths:
        protocol = path.rsplit("/", 1)[-1]
        source = f"{protocol_root}/{path}.xml"
        header = f"$(B)/protocol/{protocol}-client-protocol.h"
        code = f"$(B)/protocol/{protocol}-client-protocol-code.h"
        protocol_outputs += [header, code]
        protocol_commands += [
            ["wayland-scanner", "client-header", source, header],
            ["wayland-scanner", "private-code", source, code],
        ]
        if protocol in {
            "xdg-shell",
            "viewporter",
            "fractional-scale-v1",
            "xdg-decoration-unstable-v1",
            "xdg-activation-v1",
            "primary-selection-unstable-v1",
            "tablet-unstable-v2",
            "text-input-unstable-v3",
            "cursor-shape-v1",
        }:
            server_header = f"$(B)/protocol/{protocol}-server-protocol.h"
            server_protocol_outputs.append(server_header)
            protocol_outputs.append(server_header)
            protocol_commands.append(
                ["wayland-scanner", "server-header", source, server_header],
            )
    protocols = command(
        inputs=[f"{protocol_root}/{path}.xml" for path in protocol_paths],
        outputs=protocol_outputs,
        cmd=protocol_commands,
        cflags=["-I$(B)/protocol"],
        descr="WL",
        color="blue",
    )
    backend_source = {
        "src": "$(S)/platform_wayland.cpp",
        "inputs": protocol_outputs,
    }
    backend_deps = [
        protocols,
        pkg_config("wayland-client >= 1.20"),
        pkg_config("xkbcommon >= 1.0"),
        dependency(ldflags=["-lrt", "-lpthread"]),
    ]
elif system == "Darwin":
    darwin_frameworks = os.path.join(os.environ["OSX_SDK"], "System", "Library", "Frameworks") if "OSX_SDK" in os.environ else None
    if darwin_frameworks:
        build.cppflags += [f"-F{darwin_frameworks}"]
    backend_source = "$(S)/platform_cocoa.mm"
    backend_cxxflags = [
        "-fobjc-arc",
        "-fblocks",
        "-Wno-availability",
        "-Wno-missing-method-return-type",
        "-Wno-unused-parameter",
        # Cross builds receive the SDK frameworks through -F, a user search
        # path, so clang diagnoses the SDK headers themselves. Xcode gets the
        # same headers as system headers and never sees these warnings.
        "-Wno-nullability-completeness",
        "-Wno-unguarded-availability-new",
    ]
    backend_deps = [
        # Single-token -Wl,-framework,X spellings: the graph deduplicates
        # ldflags tokens, and separate "-framework" words collapse into one.
        dependency(ldflags=[
            *([f"-F{darwin_frameworks}"] if darwin_frameworks else []),
            "-Wl,-framework,AppKit",
            "-Wl,-framework,Carbon",
            "-Wl,-framework,CoreGraphics",
            "-Wl,-framework,CoreVideo",
            "-Wl,-framework,Metal",
            "-Wl,-framework,QuartzCore",
        ]),
    ]
else:
    raise RuntimeError(f"unsupported platform: {system}")

libplt = library(
    name="plt",
    srcs=[*common_sources, backend_source],
    public_cflags=["-I$(S)", "-I$(S)/.."],
    cxxflags=locals().get("backend_cxxflags", []),
    deps=[libstd, *backend_deps],
    output="$(B)/libplt.a",
)

if build.target == build.host:
    plt_unit_test_sources = [
        "$(S)/tests/test_ut.cpp",
        "$(S)/drop_ut.cpp",
        "$(S)/fiber_ut.cpp",
        "$(S)/mutex_ut.cpp",
        "$(S)/platform_headless_ut.cpp",
        "$(S)/pointer_grab_ut.cpp",
    ]
    if system == "Darwin":
        plt_unit_test_sources.append("$(S)/platform_cocoa_ut.mm")
    plt_unit_tests = program(
        name="plt_unit_tests",
        output="$(B)/plt_unit_tests",
        srcs=plt_unit_test_sources,
        deps=[libplt, libstd],
    )

    # Hard per-invocation timeout so a hung test cannot wedge the whole CI run.
    test_timeout = ["python3", "$(S)/tests/run_timed.py", "120"]
    test_deps = [plt_unit_tests]
    test_commands = [[*test_timeout, "$(B)/plt_unit_tests"]]
    if system == "Linux":
        wayland_test_sources = [
            "$(S)/tests/test.cpp",
            *sorted(build.glob("$(S)/tests/test_wayland_*.cpp")),
        ]
        plt_wayland_integration_tests = program(
            name="plt_wayland_integration_tests",
            output="$(B)/plt_wayland_integration_tests",
            srcs=[
                {
                    "src": source,
                    "inputs": server_protocol_outputs,
                }
                for source in wayland_test_sources
            ],
            deps=[
                libplt,
                libstd,
                pkg_config("wayland-server >= 1.20"),
                pkg_config("xkbcommon >= 1.0"),
            ],
        )
        test_deps.append(plt_wayland_integration_tests)
        test_commands.append([*test_timeout, "$(B)/plt_wayland_integration_tests"])

    plt_tests = command(
        name="plt_tests",
        inputs=["$(S)/tests/run_timed.py"],
        outputs=["$(B)/plt_tests.stamp"],
        deps=test_deps,
        cmd=[
            *test_commands,
            [
                "python3", "-c",
                "from pathlib import Path; Path(r'$(B)/plt_tests.stamp').touch()",
            ],
        ],
        descr="TS",
        color="green",
    )
    group("test", plt_tests)

install(libplt)
