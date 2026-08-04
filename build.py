import hashlib
import json
import os
import subprocess
from datetime import date
from pathlib import Path

import build


std_build = os.path.join("third_party", "libstd", "build.py")
plt_build = os.path.join("third_party", "plt", "build.py")
shitty_version = os.environ.get("SHITTY_VERSION", date.today().strftime("%Y.%m.%d"))

build.flags.allow({
    "group": {
        "descr": "zero-based test partition to include",
        "default": "",
    },
    "group_count": {
        "descr": "total number of test partitions",
        "default": "",
    },
})


def parse_test_partition():
    group_value = build.flags.group
    group_count_value = build.flags.group_count
    if bool(group_value) != bool(group_count_value):
        raise RuntimeError("-Dgroup and -Dgroup_count must be specified together")
    if not group_value:
        return None
    try:
        group_index = int(group_value)
        group_count = int(group_count_value)
    except ValueError as error:
        raise RuntimeError("-Dgroup and -Dgroup_count must be integers") from error
    if group_count <= 0 or group_index < 0 or group_index >= group_count:
        raise RuntimeError(
            "test partition requires 0 <= group < group_count and group_count > 0"
        )
    return group_index, group_count


test_partition = parse_test_partition()
test_ids = set()


def add_test(*targets):
    for target in targets:
        test_id = target.name or target.output or "\0".join(target.outputs)
        if not test_id:
            raise RuntimeError("test target has no deterministic identifier")
        if test_id in test_ids:
            raise RuntimeError(f"test target added twice: {test_id}")
        test_ids.add(test_id)
        if test_partition is not None:
            group_index, group_count = test_partition
            digest = hashlib.sha256(test_id.encode()).digest()
            if int.from_bytes(digest[:8], "big") % group_count != group_index:
                continue
        group("test", target)


build.includes += ["$(B)", "$(S)/third_party"]
build.cppflags += [f'-DSHITTY_VERSION="{shitty_version}"']
# libstd needs -std=c++26, which the Apple command-line-tools clang does
# not know; fail here with directions instead of deep inside the graph.
cxx = os.environ.get("CXX", "c++")  # the runner's own compiler default
if subprocess.run(
    [cxx, "-std=c++26", "-fsyntax-only", "-x", "c++", os.devnull],
    capture_output=True,
).returncode != 0:
    raise RuntimeError(
        f"{cxx} does not accept -std=c++26 (Apple clang from the "
        "command line tools is too old). Install a current LLVM and point "
        "the build at it:\n"
        "    brew install llvm\n"
        '    export CC="$(brew --prefix llvm)/bin/clang"\n'
        '    export CXX="$(brew --prefix llvm)/bin/clang++"\n'
        "    ./build"
    )

build.cxxflags += [
    "-std=c++23",
    "-Og" if "-DDEBUG" in build.cppflags else "-O2",
]


darwin = "apple-darwin" in build.target
linux = "linux" in build.target


untimed_command = command

def command(**kwargs):
    # Hard per-invocation timeout so one hung test cannot wedge the whole CI
    # run. Only test invocations are wrapped: build steps (ragel, shaders,
    # helper binaries) run untimed.
    def is_test(argv):
        if argv[0] == "$(B)/unit_tests":
            return True
        return argv[0] == "python3" and len(argv) > 1 and (
            argv[1].startswith("tests/") or argv[1] == "-m"
        )

    cmd = kwargs.get("cmd")
    if cmd:
        nested = cmd if isinstance(cmd[0], list) else [cmd]
        if any(is_test(argv) for argv in nested):
            kwargs["cmd"] = [
                ["python3", "$(S)/tests/run_timed.py", "60", *argv]
                if is_test(argv) else argv
                for argv in nested
            ]
            kwargs["inputs"] = [*kwargs.get("inputs", []), "$(S)/tests/run_timed.py"]
    return untimed_command(**kwargs)

freetype = pkg_config("freetype2", required=False)
fontconfig = pkg_config("fontconfig", required=False)
harfbuzz = pkg_config("harfbuzz", required=False)
brotli_common = pkg_config("libbrotlicommon", required=False)
simdutf = pkg_config("simdutf >= 6.5.0", required=False)

have_freetype_backend = bool(freetype and harfbuzz)
if have_freetype_backend:
    build.cppflags += ["-DHAVE_FREETYPE=1", "-DHAVE_HARFBUZZ=1"]
    if fontconfig:
        build.cppflags += ["-DHAVE_FONTCONFIG=1"]
else:
    freetype = dependency()
    fontconfig = dependency()
    harfbuzz = dependency()
    brotli_common = dependency()

if darwin:
    darwin_frameworks = os.path.join(os.environ["OSX_SDK"], "System", "Library", "Frameworks") if "OSX_SDK" in os.environ else None
    darwin_backend = dependency(ldflags=[
        *([f"-F{darwin_frameworks}"] if darwin_frameworks else []),
        "-Wl,-ObjC",
        "-Wl,-framework,AppKit",
        "-Wl,-framework,Carbon",
        "-Wl,-framework,CoreFoundation",
        "-Wl,-framework,CoreGraphics",
        "-Wl,-framework,CoreText",
        "-Wl,-framework,Foundation",
        "-Wl,-framework,IOSurface",
        "-Wl,-framework,Metal",
        "-Wl,-framework,QuartzCore",
    ])
    if darwin_frameworks:
        build.cppflags += [f"-F{darwin_frameworks}"]
    build.cppflags += ["-DHAVE_CORETEXT=1", "-DHAVE_METAL_RENDERER=1"]
else:
    darwin_backend = dependency()

# >= 2.9: grapheme.cpp uses the Indic_Conjunct_Break property API.
utf8proc = pkg_config("libutf8proc >= 2.9.0")
threads = dependency(ldflags=["-pthread"])

vulkan = dependency()
wayland = dependency()
xkbcommon = dependency()
realtime = dependency()
math = dependency()
cxx_runtime = dependency()
if linux:
    vulkan = pkg_config("vulkan")
    wayland = pkg_config("wayland-client >= 1.20")
    xkbcommon = pkg_config("xkbcommon >= 1.0")
    realtime = dependency(ldflags=["-lrt"])
    math = dependency(ldflags=["-lm"])
    cxx_runtime = dependency(ldflags=["-lstdc++"])
    build.cppflags += ["-DHAVE_VULKAN_WAYLAND=1"]


libstd = import_build(std_build, "libstd.a", extra_cflags=["-Wno-error"])
libstd_external_clock = import_build(
    std_build,
    "libstd_external_clock.a",
    extra_cflags=["-Wno-error"],
    extra_cppflags=["-DSTL_EXTERNAL_MONOTONIC_NOW_US=1"],
)


if "-lplt" in build.ldflags:
    plt = dependency(ldflags=["-lplt"])
elif os.path.isfile(os.path.join(os.path.dirname(__file__), plt_build)):
    plt = import_build(
        plt_build,
        "libplt.a",
        extra_cflags=["-Wno-error"],
        extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
    )
else:
    plt = dependency(ldflags=["-lplt"])


# plt ships its own test suite (unit tests plus fake-compositor integration
# scenarios); nothing else runs it, so import its test programs into this
# graph and stamp them into the test group. They compile against our bundled
# libstd and link the archive built by this graph.
if build.target == build.host and os.path.isfile(os.path.join(os.path.dirname(__file__), plt_build)):
    plt_test_programs = [
        import_build(
            plt_build,
            "plt_unit_tests",
            extra_cflags=["-Wno-error"],
            extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
            deps=[libstd],
        ),
    ]
    if linux:
        plt_test_programs.append(import_build(
            plt_build,
            "plt_wayland_integration_tests",
            extra_cflags=["-Wno-error"],
            extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
            deps=[libstd],
        ))
    plt_tests = untimed_command(
        name="plt_tests",
        inputs=["$(S)/third_party/plt/tests/run_timed.py"],
        outputs=["$(B)/plt-tests.stamp"],
        deps=plt_test_programs,
        cmd=[
            # The same hard per-invocation timeout the nested suite uses.
            *[
                ["python3", "$(S)/third_party/plt/tests/run_timed.py", "120", program.output]
                for program in plt_test_programs
            ],
            [
                "python3", "-c",
                "from pathlib import Path; Path(r'$(B)/plt-tests.stamp').touch()",
            ],
        ],
        descr="PT",
        color="green",
    )
else:
    plt_tests = None


render_shader_names = [
    "rgba8_unorm",
    "bgra8_unorm",
    "a8b8g8r8_unorm",
    "rgba8_srgb",
    "bgra8_srgb",
    "a8b8g8r8_srgb",
    "a2b10g10r10_unorm",
    "a2r10g10b10_unorm",
    "rgba16_unorm",
    "rgba16_sfloat_linear",
    "r5g6b5_unorm",
    "b5g6r5_unorm",
    "r4g4b4a4_unorm",
    "b4g4r4a4_unorm",
    "r5g5b5a1_unorm",
    "b5g5r5a1_unorm",
    "a1r5g5b5_unorm",
]
render_shader_outputs = []
render_shader_targets = []
for render_shader_name in render_shader_names:
    render_shader_output = f"$(B)/render_shader_{render_shader_name}.inc"
    render_shader_outputs.append(render_shader_output)
    render_shader_targets.append(command(
        name=f"render_shader_{render_shader_name}",
        inputs=["$(S)/render.comp", "$(S)/generate_render_shaders.py"],
        outputs=[render_shader_output],
        cmd=[
            "python3",
            "$(S)/generate_render_shaders.py",
            "compile",
            "$(S)/render.comp",
            render_shader_name,
            render_shader_output,
            "glslangValidator",
        ],
        descr="SH",
        color="magenta",
    ))

render_spv = command(
    name="render_spv",
    inputs=["$(S)/generate_render_shaders.py", *render_shader_outputs],
    outputs=["$(B)/render_spv.h"],
    deps=render_shader_targets,
    cmd=[
        "python3",
        "$(S)/generate_render_shaders.py",
        "combine",
        "$(B)/render_spv.h",
        *render_shader_outputs,
    ],
    descr="SH",
    color="magenta",
)

if darwin:
    render_msl = command(
        name="render_msl",
        inputs=["$(S)/render.comp", "$(S)/generate_render_shaders.py"],
        outputs=["$(B)/render_msl.h"],
        cmd=[
            "python3",
            "$(S)/generate_render_shaders.py",
            "metal",
            "$(S)/render.comp",
            "$(B)/render_msl.h",
            "glslangValidator",
            "spirv-cross",
        ],
        descr="SH",
        color="magenta",
    )

parser_totality = command(
    name="parser_totality",
    inputs=["$(S)/parser.rl", "$(S)/check_parser_totality.py"],
    outputs=["$(B)/parser.rl.total"],
    cmd=[
        "python3",
        "$(S)/check_parser_totality.py",
        "$(S)/parser.rl",
        "$(B)/parser.rl.total",
    ],
    descr="RG",
    color="magenta",
)

parser_prod = command(
    name="parser_prod",
    inputs=["$(S)/parser.rl"],
    outputs=["$(B)/parser.rl.h"],
    deps=[parser_totality],
    cmd=[
        "ragel",
        "-C",
        "-G1",
        "-L",
        "-o",
        "$(B)/parser.rl.h",
        "$(S)/parser.rl",
    ],
    descr="RG",
    color="magenta",
)

# No totality check here: unlike the VT stream, the config parser is allowed
# to reject input, so unhandled bytes are ordinary syntax errors.
toml_prod = command(
    name="toml_prod",
    inputs=["$(S)/toml.rl"],
    outputs=["$(B)/toml.rl.h"],
    cmd=[
        "ragel",
        "-C",
        "-G1",
        "-L",
        "-o",
        "$(B)/toml.rl.h",
        "$(S)/toml.rl",
    ],
    descr="RG",
    color="magenta",
)

parser_test = command(
    name="parser_test",
    inputs=["$(S)/parser.rl"],
    outputs=["$(B)/parser_test.rl.h"],
    deps=[parser_totality],
    cmd=[
        "ragel",
        "-C",
        "-T1",
        "-L",
        "-o",
        "$(B)/parser_test.rl.h",
        "$(S)/parser.rl",
    ],
    descr="RG",
    color="magenta",
)


utf8_dfa = command(
    name="utf8_dfa",
    inputs=["$(S)/generate_utf8_dfa.py"],
    outputs=["$(B)/utf8_dfa.h"],
    cmd=[
        "python3",
        "$(S)/generate_utf8_dfa.py",
        "$(B)/utf8_dfa.h",
    ],
    descr="DF",
    color="magenta",
)


icon_png = command(
    name="icon_png",
    inputs=["$(S)/shitty.svg"],
    outputs=["$(B)/shitty.png"],
    cmd=[
        "rsvg-convert",
        "-w", "1024",
        "-h", "1024",
        "$(S)/shitty.svg",
        "-o", "$(B)/shitty.png",
    ],
    descr="SV",
    color="magenta",
)


icon_data = command(
    name="icon_data",
    inputs=[
        "$(S)/generate_font_data.py",
        "$(B)/shitty.png",
    ],
    deps=[icon_png],
    outputs=["$(B)/icon_data.h"],
    cmd=[
        "python3",
        "$(S)/generate_font_data.py",
        "$(B)/icon_data.h",
        "embeddedIcon=$(B)/shitty.png",
    ],
    descr="IC",
    color="magenta",
)


font_data = command(
    name="font_data",
    inputs=[
        "$(S)/generate_font_data.py",
        "$(S)/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/fonts/NotoColorEmoji.ttf",
        "$(S)/fonts/NotoEmoji-Regular.ttf",
    ],
    outputs=["$(B)/font_data.h"],
    cmd=[
        "python3",
        "$(S)/generate_font_data.py",
        "$(B)/font_data.h",
        "embeddedFontMono=$(S)/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "embeddedFontEmoji=$(S)/fonts/NotoColorEmoji.ttf",
        "embeddedFontEmojiText=$(S)/fonts/NotoEmoji-Regular.ttf",
    ],
    descr="FD",
    color="magenta",
)


main_source = "$(S)/main.cpp"
fuzz_source = "$(S)/main_fuzz.cpp"
heap_profile_source = "$(S)/heap_profile.cpp"
parser_source = "$(S)/parser.cpp"
toml_source = "$(S)/toml.cpp"
toml_dump_source = "$(S)/toml_dump.cpp"
unit_sources = sorted(build.glob("$(S)/*_ut.cpp"))
platform_font_sources = {
    "$(S)/font_freetype.cpp",
}
platform_renderer_sources = {
    "$(S)/render_vk.cpp",
}
enabled_font_sources = set()
if have_freetype_backend:
    enabled_font_sources.add("$(S)/font_freetype.cpp")
enabled_renderer_sources = set()
if linux:
    enabled_renderer_sources.add("$(S)/render_vk.cpp")
all_libshitty_sources = [
    source for source in build.glob("$(S)/*.cpp")
    if source not in (main_source, fuzz_source, heap_profile_source, toml_dump_source, *unit_sources)
    and (source not in platform_font_sources or source in enabled_font_sources)
    and (source not in platform_renderer_sources or source in enabled_renderer_sources)
]
if darwin:
    all_libshitty_sources.append({
        "src": "$(S)/render_metal.mm",
        "inputs": ["$(B)/render_msl.h"],
    })
vterm_source = "$(S)/vterm.cpp"
font_embedded_source = "$(S)/font_embedded.cpp"
application_source = "$(S)/application.cpp"
libshitty_sources = [
    {
        "src": source,
        "inputs": ["$(B)/parser.rl.h"],
    } if source == parser_source else {
        "src": source,
        "inputs": ["$(B)/toml.rl.h"],
    } if source == toml_source else {
        "src": source,
        "inputs": ["$(B)/utf8_dfa.h"],
    } if source == vterm_source else {
        "src": source,
        "inputs": ["$(B)/font_data.h"],
    } if source == font_embedded_source else {
        "src": source,
        "inputs": ["$(B)/icon_data.h"],
    } if source == application_source else source
    for source in all_libshitty_sources
]
libshitty_test_sources = [
    {
        "src": source,
        "inputs": ["$(B)/parser_test.rl.h"],
    } if source == parser_source else {
        "src": source,
        "inputs": ["$(B)/toml.rl.h"],
    } if source == toml_source else {
        "src": source,
        "inputs": ["$(B)/utf8_dfa.h"],
    } if source == vterm_source else {
        "src": source,
        "inputs": ["$(B)/font_data.h"],
    } if source == font_embedded_source else {
        "src": source,
        "inputs": ["$(B)/icon_data.h"],
    } if source == application_source else source
    for source in all_libshitty_sources
]
libshitty_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, threads, libstd,
    brotli_common, utf8proc, simdutf, wayland, xkbcommon, realtime, math, cxx_runtime,
]
libshitty_test_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, threads, libstd,
    brotli_common, utf8proc, simdutf, wayland, xkbcommon, realtime, math, cxx_runtime,
]
libshitty_fuzz_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, threads, libstd_external_clock,
    brotli_common, utf8proc, simdutf, wayland, xkbcommon, realtime, math, cxx_runtime,
]


libshitty = library(
    srcs=libshitty_sources,
    deps=libshitty_deps,
    output="$(B)/libshitty_prod.a",
)


st = program(
    srcs=[main_source],
    deps=[libshitty],
)


heap_profile_cxxflags = [
    "-g",
    "-fno-omit-frame-pointer",
    "-mno-omit-leaf-frame-pointer",
]


libshitty_memprofile = library(
    name="libshitty_memprofile",
    srcs=libshitty_sources,
    cxxflags=heap_profile_cxxflags,
    deps=libshitty_deps,
    output="$(B)/libshitty_memprofile.a",
)


st_memprofile = program(
    name="st_memprofile",
    output="$(B)/st_memprofile",
    srcs=[main_source, heap_profile_source],
    cxxflags=heap_profile_cxxflags,
    cppflags=["-DSHITTY_HEAP_PROFILE=1"],
    deps=[libshitty_memprofile],
)


# The control protocol is compiled into both binaries. SHITTY_FOR_TESTS only
# opens its application entry point and exposes Vterm::testApi().
libshitty_test = library(
    name="libshitty_test",
    srcs=libshitty_test_sources,
    cppflags=["-DSHITTY_FOR_TESTS=1", "-DSHITTY_COMPACT_PARSER=1"],
    deps=libshitty_test_deps,
    output="$(B)/libshitty_test.a",
)


# The fuzz target owns monotonicNowUs() so it can advance time exactly one
# record at a time. Its libstd variant deliberately omits the default clock.
libshitty_fuzz = library(
    name="libshitty_fuzz",
    srcs=libshitty_test_sources,
    cppflags=["-DSHITTY_FOR_TESTS=1", "-DSHITTY_COMPACT_PARSER=1"],
    deps=libshitty_fuzz_deps,
    output="$(B)/libshitty_fuzz.a",
)


st_test = program(
    name="st_test",
    output="$(B)/st_test",
    srcs=[main_source],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test],
)


main_fuzz = program(
    srcs=[fuzz_source],
    deps=[libshitty_fuzz],
)


# Same test build against the production ragel backend (-G1): the two
# backends share the C++ semantics but not the generated code, and only
# this variant executes what ships in st.
libshitty_test_prod_parser = library(
    name="libshitty_test_prod_parser",
    srcs=libshitty_sources,
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=libshitty_test_deps,
    output="$(B)/libshitty_test_prod_parser.a",
)


st_test_prod_parser = program(
    name="st_test_prod_parser",
    output="$(B)/st_test_prod_parser",
    srcs=[main_source],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test_prod_parser],
)


unit_tests = program(
    name="unit_tests",
    output="$(B)/unit_tests",
    srcs=["$(S)/third_party/libstd/tst/test.cpp", *unit_sources],
    deps=[libshitty_test, libstd],
)


toml_dump = program(
    name="toml_dump",
    output="$(B)/toml_dump",
    srcs=[toml_dump_source],
    deps=[libshitty_test, libstd],
)


# Each shard is an independent graph node with its own hard timeout.
test_group_count = 20
python_test_inputs = [
    *build.glob("$(S)/tests/*.py"),
    *build.glob("$(S)/tests/toml/*/*/*"),
    "$(S)/tests/windows_terminal/upstream/KittyKeyboardProtocol.cpp",
    "$(S)/tests/windows_terminal/upstream/ReflowTests.cpp",
    "$(S)/tests/windows_terminal/upstream/ScreenBufferTests.cpp",
    "$(S)/tests/windows_terminal/upstream/SelectionTest.cpp",
    "$(S)/tests/windows_terminal/upstream/TerminalBufferTests.cpp",
    "$(S)/application.cpp",
    "$(S)/shitty.desktop",
]


def touch_stamp(path):
    return [
        "python3",
        "-c",
        f"from pathlib import Path; Path(r'{path}').touch()",
    ]


unit_test_groups = []
for group_index in range(test_group_count):
    output = f"$(B)/unit-tests/group-{group_index:02}.stamp"
    unit_test_groups.append(command(
        name=f"unit_tests_group_{group_index:02}",
        outputs=[output],
        deps=[unit_tests],
        cmd=[
            [
                "$(B)/unit_tests",
                f"--group={group_index}",
                f"--group-count={test_group_count}",
                "--threads=1",
            ],
            touch_stamp(output),
        ],
        descr="UT",
        color="green",
    ))


def make_python_test_groups(name, output_directory, test_binary, test_target, descr):
    result = []

    for group_index in range(test_group_count):
        output = f"$(B)/{output_directory}/group-{group_index:02}.stamp"
        result.append(command(
            name=f"{name}_group_{group_index:02}",
            inputs=python_test_inputs,
            outputs=[output],
            deps=[test_target, st, toml_dump],
            cmd=[
                [
                    "python3",
                    "tests/run_unittest_group.py",
                    f"--group={group_index}",
                    f"--group-count={test_group_count}",
                ],
                touch_stamp(output),
            ],
            cwd="$(S)",
            env={
                "SHITTY_TEST_BINARY": test_binary,
                "SHITTY_TOML_DUMP_BINARY": "$(B)/toml_dump",
                "SHITTY_TEST_FONTCONFIG": "1" if fontconfig else "0",
                "SHITTY_TEST_PLATFORM": "cocoa" if darwin else "wayland",
                "SHITTY_TEST_VERSION": shitty_version,
                "SHITTY_PRODUCTION_BINARY": "$(B)/st",
            },
            descr=descr,
            color="cyan",
        ))

    return result


python_test_groups = make_python_test_groups(
    "test_suite",
    "python-tests",
    "$(B)/st_test",
    st_test,
    "TS",
)
python_test_prod_parser_groups = make_python_test_groups(
    "test_suite_prod_parser",
    "python-tests-prod-parser",
    "$(B)/st_test_prod_parser",
    st_test_prod_parser,
    "TP",
)


test_suite = untimed_command(
    inputs=["$(S)/build.py"],
    outputs=["$(B)/tests.stamp"],
    deps=[*unit_test_groups, *python_test_groups],
    cmd=touch_stamp("$(B)/tests.stamp"),
    descr="TS",
    color="cyan",
)


test_suite_prod_parser = untimed_command(
    name="test_suite_prod_parser",
    inputs=["$(S)/build.py"],
    outputs=["$(B)/tests-prod-parser.stamp"],
    deps=python_test_prod_parser_groups,
    cmd=touch_stamp("$(B)/tests-prod-parser.stamp"),
    descr="TP",
    color="cyan",
)


parser_fuzz = command(
    inputs=["$(S)/tests/fuzz_parser.py", "$(S)/tests/harness.py"],
    outputs=["$(B)/parser-fuzz.stamp"],
    deps=[st_test],
    cmd=[
        ["python3", "tests/fuzz_parser.py"],
        [
            "python3", "-c",
            "from pathlib import Path; Path(r'$(B)/parser-fuzz.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
    descr="FZ",
    color="yellow",
)


vttest_profile = command(
    inputs=["$(S)/tests/vttest.py", "$(S)/tests/harness.py"],
    outputs=["$(B)/vttest.stamp"],
    deps=[st_test],
    cmd=[
        ["python3", "tests/vttest.py"],
        [
            "python3", "-c",
            "from pathlib import Path; Path(r'$(B)/vttest.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
    descr="VT",
    color="blue",
)


xtermjs_root = Path(__file__).parent / "tests" / "xtermjs"
xtermjs_cases = (xtermjs_root / "file_names.txt").read_text().split()
xtermjs_tests = []
for case in xtermjs_cases:
    xtermjs_tests.append(command(
        name="xtermjs_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/xtermjs/adapter.py",
            "$(S)/tests/xtermjs/file_names.txt",
            "$(S)/tests/xtermjs/xfail.txt",
            f"$(S)/tests/xtermjs/{case}.in",
            f"$(S)/tests/xtermjs/{case}.text",
        ],
        outputs=[f"$(B)/tests/xtermjs/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/xtermjs/adapter.py",
            case,
            "tests/xtermjs/xfail.txt",
            f"$(B)/tests/xtermjs/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="XJ",
        color="cyan",
    ))


alacritty_root = Path(__file__).parent / "tests" / "alacritty"
alacritty_cases = (alacritty_root / "file_names.txt").read_text().split()
alacritty_tests = []
for case in alacritty_cases:
    alacritty_tests.append(command(
        name="alacritty_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/alacritty/adapter.py",
            "$(S)/tests/alacritty/file_names.txt",
            "$(S)/tests/alacritty/xfail.txt",
            f"$(S)/tests/alacritty/{case}/alacritty.recording",
            f"$(S)/tests/alacritty/{case}/config.json",
            f"$(S)/tests/alacritty/{case}/grid.json",
            f"$(S)/tests/alacritty/{case}/size.json",
        ],
        outputs=[f"$(B)/tests/alacritty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/alacritty/adapter.py",
            case,
            "tests/alacritty/xfail.txt",
            f"$(B)/tests/alacritty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="AL",
        color="cyan",
    ))


contour_vttest_sources = [
    source for source in build.glob("$(S)/tests/contour/vttest/*.c")
    if not source.endswith("/vms_io.c")
]
contour_vttest = program(
    name="contour_vttest_helper",
    srcs=contour_vttest_sources,
    cflags=["-Wno-error"],
    cppflags=[
        "-DHAVE_CONFIG_H",
        "-I$(S)/tests/contour/vttest",
    ],
    output="$(B)/tests/contour/vttest",
)


contour_root = Path(__file__).parent / "tests" / "contour"
contour_cases = (contour_root / "file_names.txt").read_text().split()
contour_tests = []
for case in contour_cases:
    golden_inputs = [
        "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
        for path in sorted((contour_root / "golden").glob(f"{case}.step*.dump"))
    ]
    contour_tests.append(command(
        name="contour_" + case.replace(".", "_"),
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/contour/adapter.py",
            "$(S)/tests/contour/file_names.txt",
            "$(S)/tests/contour/scenarios.json",
            "$(S)/tests/contour/xfail.txt",
            *golden_inputs,
        ],
        outputs=[f"$(B)/tests/contour/{case}.stamp"],
        deps=[st_test, contour_vttest],
        cmd=[
            "python3",
            "tests/contour/adapter.py",
            "$(B)/tests/contour/vttest",
            case,
            "tests/contour/xfail.txt",
            f"$(B)/tests/contour/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="CO",
        color="cyan",
    ))


mosh_tests = []
for corpus in ("terminal_corpus", "terminal_parser_corpus"):
    mosh_tests.append(command(
        name="mosh_" + corpus,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/mosh/adapter.py",
            "$(S)/tests/mosh/xfail.txt",
            *build.glob(f"$(S)/tests/mosh/{corpus}/*"),
        ],
        outputs=[f"$(B)/tests/mosh/{corpus}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/mosh/adapter.py",
            corpus,
            "tests/mosh/xfail.txt",
            f"$(B)/tests/mosh/{corpus}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="MO",
        color="cyan",
    ))

mosh_root = Path(__file__).parent / "tests" / "mosh"
mosh_semantic_cases = (
    mosh_root / "semantic_file_names.txt"
).read_text().split()
mosh_semantic_tests = []
for case in mosh_semantic_cases:
    mosh_semantic_tests.append(command(
        name="mosh_semantic_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/mosh/semantic_adapter.py",
            "$(S)/tests/mosh/semantic_cases.py",
            "$(S)/tests/mosh/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tests/mosh/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/mosh/semantic_adapter.py",
            case,
            f"$(B)/tests/mosh/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="MO",
        color="cyan",
    ))

mosh_semantic_validation = command(
    name="mosh_semantic_catalog",
    inputs=[
        "$(S)/tests/mosh/semantic_cases.py",
        "$(S)/tests/mosh/semantic_file_names.txt",
        "$(S)/tests/mosh/semantic_validate.py",
    ],
    outputs=["$(B)/tests/mosh/semantic/catalog.stamp"],
    cmd=[
        ["python3", "tests/mosh/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/mosh/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="MO",
    color="cyan",
)


libtsm_root = Path(__file__).parent / "tests" / "libtsm"
libtsm_semantic_cases = (
    libtsm_root / "semantic_file_names.txt"
).read_text().split()
libtsm_semantic_tests = []
for case in libtsm_semantic_cases:
    libtsm_semantic_tests.append(command(
        name="libtsm_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/libtsm/semantic_adapter.py",
            "$(S)/tests/libtsm/semantic_cases.py",
            "$(S)/tests/libtsm/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tests/libtsm/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/libtsm/semantic_adapter.py",
            case,
            f"$(B)/tests/libtsm/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="LT",
        color="cyan",
    ))

libtsm_semantic_validation = command(
    name="libtsm_semantic_catalog",
    inputs=[
        "$(S)/tests/libtsm/semantic_cases.py",
        "$(S)/tests/libtsm/semantic_file_names.txt",
        "$(S)/tests/libtsm/semantic_validate.py",
    ],
    outputs=["$(B)/tests/libtsm/catalog.stamp"],
    cmd=[
        ["python3", "tests/libtsm/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/libtsm/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="LT",
    color="cyan",
)


ghostty_root = Path(__file__).parent / "tests" / "ghostty"
ghostty_members = []
for corpus in ("osc-cmin", "parser-cmin", "stream-cmin"):
    ghostty_members.extend(
        f"{corpus}/{path.name}"
        for path in sorted((ghostty_root / corpus).iterdir())
    )
ghostty_xfails = {
    line.strip()
    for line in (ghostty_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_ghostty_xfails = ghostty_xfails - set(ghostty_members)
if unknown_ghostty_xfails:
    raise RuntimeError(
        "unknown Ghostty XFAIL members: "
        + ", ".join(sorted(unknown_ghostty_xfails))
    )

ghostty_tests = []
ghostty_shard_size = 128
for corpus in ("osc-cmin", "parser-cmin", "stream-cmin"):
    members = [
        member for member in ghostty_members
        if member.startswith(corpus + "/")
    ]
    for shard_index, start in enumerate(range(0, len(members), ghostty_shard_size)):
        shard = members[start : start + ghostty_shard_size]
        name = corpus.replace("-", "_") + f"_{shard_index:03d}"
        ghostty_tests.append(command(
            name="ghostty_" + name,
            inputs=[
                "$(S)/tests/harness.py",
                "$(S)/tests/fuzz_parser.py",
                "$(S)/tests/ghostty/adapter.py",
                "$(S)/tests/ghostty/xfail.txt",
                *("$(S)/tests/ghostty/" + member for member in shard),
            ],
            outputs=[f"$(B)/tests/ghostty/{name}.stamp"],
            deps=[st_test],
            cmd=[
                "python3",
                "tests/ghostty/adapter.py",
                "tests/ghostty/xfail.txt",
                f"$(B)/tests/ghostty/{name}.stamp",
                *shard,
            ],
            cwd="$(S)",
            env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
            descr="GH",
            color="cyan",
        ))


ghostty_semantic_cases = (
    ghostty_root / "semantic_file_names.txt"
).read_text().split()
ghostty_semantic_tests = []
for case in ghostty_semantic_cases:
    ghostty_semantic_tests.append(command(
        name="ghostty_model_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/ghostty/semantic_adapter.py",
            "$(S)/tests/ghostty/semantic_catalog.py",
            "$(S)/tests/ghostty/semantic_file_names.txt",
            "$(S)/tests/ghostty/semantic_xfail.txt",
            "$(S)/tests/ghostty/upstream/stream_terminal_tests.zig",
        ],
        outputs=[f"$(B)/tests/ghostty/model/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/ghostty/semantic_adapter.py",
            case,
            "tests/ghostty/semantic_xfail.txt",
            f"$(B)/tests/ghostty/model/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="GH",
        color="cyan",
    ))


ghostty_semantic_validation = command(
    name="ghostty_model_catalog",
    inputs=[
        "$(S)/tests/ghostty/semantic_catalog.py",
        "$(S)/tests/ghostty/semantic_file_names.txt",
        "$(S)/tests/ghostty/semantic_validate.py",
        "$(S)/tests/ghostty/semantic_xfail.txt",
        "$(S)/tests/ghostty/upstream/stream_terminal_tests.zig",
    ],
    outputs=["$(B)/tests/ghostty/model/catalog.stamp"],
    cmd=[
        ["python3", "tests/ghostty/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/ghostty/model/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="GH",
    color="cyan",
)


kitty_root = Path(__file__).parent / "tests" / "kitty"
kitty_cases = (kitty_root / "file_names.txt").read_text().split()
kitty_tests = []
for case in kitty_cases:
    kitty_tests.append(command(
        name="kitty_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/kitty/adapter.py",
            "$(S)/tests/kitty/catalog.py",
            "$(S)/tests/kitty/file_names.txt",
            "$(S)/tests/kitty/xfail.txt",
            "$(S)/tests/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tests/kitty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/kitty/adapter.py",
            case,
            "tests/kitty/xfail.txt",
            f"$(B)/tests/kitty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KI",
        color="cyan",
    ))


kitty_validation = command(
    name="kitty_catalog",
    inputs=[
        "$(S)/tests/kitty/catalog.py",
        "$(S)/tests/kitty/file_names.txt",
        "$(S)/tests/kitty/validate.py",
        "$(S)/tests/kitty/xfail.txt",
        "$(S)/tests/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tests/kitty/catalog.stamp"],
    cmd=[
        ["python3", "tests/kitty/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/kitty/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KI",
    color="cyan",
)


kitty_screen_cases = (
    kitty_root / "screen_file_names.txt"
).read_text().split()
kitty_screen_tests = []
for case in kitty_screen_cases:
    kitty_screen_tests.append(command(
        name="kitty_screen_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/kitty/screen_adapter.py",
            "$(S)/tests/kitty/screen_catalog.py",
            "$(S)/tests/kitty/screen_file_names.txt",
            "$(S)/tests/kitty/screen_xfail.txt",
            "$(S)/tests/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tests/kitty/screen/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/kitty/screen_adapter.py",
            case,
            "tests/kitty/screen_xfail.txt",
            f"$(B)/tests/kitty/screen/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KS",
        color="cyan",
    ))


kitty_screen_validation = command(
    name="kitty_screen_catalog",
    inputs=[
        "$(S)/tests/kitty/screen_catalog.py",
        "$(S)/tests/kitty/screen_file_names.txt",
        "$(S)/tests/kitty/screen_validate.py",
        "$(S)/tests/kitty/screen_xfail.txt",
        "$(S)/tests/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tests/kitty/screen/catalog.stamp"],
    cmd=[
        ["python3", "tests/kitty/screen_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/kitty/screen/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KS",
    color="cyan",
)


kitty_utf8 = command(
    name="kitty_utf8",
    inputs=[
        "$(S)/tests/harness.py",
        "$(S)/tests/kitty/utf8_adapter.py",
        "$(S)/tests/kitty/utf8_catalog.py",
        "$(S)/tests/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tests/kitty/utf8.stamp"],
    deps=[st_test],
    cmd=[
        "python3",
        "tests/kitty/utf8_adapter.py",
        "$(B)/tests/kitty/utf8.stamp",
    ],
    cwd="$(S)",
    env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
    descr="KU",
    color="cyan",
)


kitty_transaction_cases = (
    kitty_root / "transaction_file_names.txt"
).read_text().split()
kitty_transaction_tests = []
for case in kitty_transaction_cases:
    kitty_transaction_tests.append(command(
        name="kitty_transaction_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/kitty/transaction_adapter.py",
            "$(S)/tests/kitty/transaction_cases.py",
            "$(S)/tests/kitty/transaction_file_names.txt",
            "$(S)/tests/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tests/kitty/transaction/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/kitty/transaction_adapter.py",
            case,
            f"$(B)/tests/kitty/transaction/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KT",
        color="cyan",
    ))

kitty_transaction_validation = command(
    name="kitty_transaction_catalog",
    inputs=[
        "$(S)/tests/harness.py",
        "$(S)/tests/kitty/transaction_cases.py",
        "$(S)/tests/kitty/transaction_file_names.txt",
        "$(S)/tests/kitty/transaction_validate.py",
        "$(S)/tests/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tests/kitty/transaction/catalog.stamp"],
    cmd=[
        ["python3", "tests/kitty/transaction_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/kitty/transaction/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KT",
    color="cyan",
)


vte_root = Path(__file__).parent / "tests" / "vte"
vte_cases = (vte_root / "file_names.txt").read_text().split()
vte_tests = []
for case in vte_cases:
    vte_tests.append(command(
        name="vte_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/adapter.py",
            "$(S)/tests/vte/catalog.py",
            "$(S)/tests/vte/file_names.txt",
            "$(S)/tests/vte/xfail.txt",
            "$(S)/tests/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/adapter.py",
            case,
            "tests/vte/xfail.txt",
            f"$(B)/tests/vte/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VE",
        color="cyan",
    ))


vte_validation = command(
    name="vte_catalog",
    inputs=[
        "$(S)/tests/vte/catalog.py",
        "$(S)/tests/vte/file_names.txt",
        "$(S)/tests/vte/validate.py",
        "$(S)/tests/vte/xfail.txt",
        "$(S)/tests/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tests/vte/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VE",
    color="cyan",
)


vte_known_cases = (vte_root / "known_file_names.txt").read_text().split()
vte_known_sources = {
    "escape": "esc",
    "csi": "csi",
    "dcs": "dcs",
}
vte_known_tests = []
for case in vte_known_cases:
    vte_known_tests.append(command(
        name="vte_known_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/known_adapter.py",
            "$(S)/tests/vte/known_cases.py",
            "$(S)/tests/vte/known_file_names.txt",
            f"$(S)/tests/vte/upstream/parser-{vte_known_sources[case]}.hh",
            "$(S)/tests/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/known/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/known_adapter.py",
            case,
            f"$(B)/tests/vte/known/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VK",
        color="cyan",
    ))


vte_known_validation = command(
    name="vte_known_catalog",
    inputs=[
        "$(S)/tests/vte/known_cases.py",
        "$(S)/tests/vte/known_file_names.txt",
        "$(S)/tests/vte/known_validate.py",
        "$(S)/tests/vte/upstream/parser-esc.hh",
        "$(S)/tests/vte/upstream/parser-csi.hh",
        "$(S)/tests/vte/upstream/parser-dcs.hh",
        "$(S)/tests/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tests/vte/known/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/known_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/known/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VK",
    color="cyan",
)


vte_charset_cases = (vte_root / "charset_file_names.txt").read_text().split()
vte_charset_tests = []
for case in vte_charset_cases:
    vte_charset_tests.append(command(
        name="vte_charset_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/charset_adapter.py",
            "$(S)/tests/vte/charset_cases.py",
            "$(S)/tests/vte/charset_file_names.txt",
            "$(S)/tests/vte/upstream/parser-charset-tables.hh",
            "$(S)/tests/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/charset/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/charset_adapter.py",
            case,
            f"$(B)/tests/vte/charset/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VC",
        color="cyan",
    ))


vte_charset_validation = command(
    name="vte_charset_catalog",
    inputs=[
        "$(S)/tests/vte/charset_cases.py",
        "$(S)/tests/vte/charset_file_names.txt",
        "$(S)/tests/vte/charset_validate.py",
        "$(S)/tests/vte/upstream/parser-charset-tables.hh",
        "$(S)/tests/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tests/vte/charset/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/charset_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/charset/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VC",
    color="cyan",
)


vte_tabstop_cases = (vte_root / "tabstop_file_names.txt").read_text().split()
vte_tabstop_tests = []
for case in vte_tabstop_cases:
    vte_tabstop_tests.append(command(
        name="vte_tabstop_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/tabstop_adapter.py",
            "$(S)/tests/vte/tabstop_cases.py",
            "$(S)/tests/vte/tabstop_file_names.txt",
            "$(S)/tests/vte/upstream/tabstops-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/tabstops/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/tabstop_adapter.py",
            case,
            f"$(B)/tests/vte/tabstops/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VT",
        color="cyan",
    ))


vte_tabstop_validation = command(
    name="vte_tabstop_catalog",
    inputs=[
        "$(S)/tests/vte/tabstop_cases.py",
        "$(S)/tests/vte/tabstop_file_names.txt",
        "$(S)/tests/vte/tabstop_validate.py",
        "$(S)/tests/vte/upstream/tabstops-test.cc",
    ],
    outputs=["$(B)/tests/vte/tabstops/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/tabstop_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/tabstops/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VT",
    color="cyan",
)


vte_mode_cases = (vte_root / "mode_file_names.txt").read_text().split()
vte_mode_tests = []
for case in vte_mode_cases:
    vte_mode_tests.append(command(
        name="vte_mode_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/mode_adapter.py",
            "$(S)/tests/vte/mode_cases.py",
            "$(S)/tests/vte/mode_file_names.txt",
            "$(S)/tests/vte/upstream/modes-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/modes/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/mode_adapter.py",
            case,
            f"$(B)/tests/vte/modes/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VM",
        color="cyan",
    ))


vte_mode_validation = command(
    name="vte_mode_catalog",
    inputs=[
        "$(S)/tests/vte/mode_cases.py",
        "$(S)/tests/vte/mode_file_names.txt",
        "$(S)/tests/vte/mode_validate.py",
        "$(S)/tests/vte/upstream/modes-test.cc",
    ],
    outputs=["$(B)/tests/vte/modes/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/mode_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/modes/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VM",
    color="cyan",
)


vte_color_cases = (vte_root / "color_file_names.txt").read_text().split()
vte_color_tests = []
for case in vte_color_cases:
    vte_color_tests.append(command(
        name="vte_color_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/color_adapter.py",
            "$(S)/tests/vte/color_cases.py",
            "$(S)/tests/vte/color_file_names.txt",
            "$(S)/tests/vte/upstream/color-test.cc",
            "$(S)/tests/vte/upstream/color-names-tests.hh",
        ],
        outputs=[f"$(B)/tests/vte/color/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/color_adapter.py",
            case,
            f"$(B)/tests/vte/color/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VC",
        color="cyan",
    ))


vte_color_validation = command(
    name="vte_color_catalog",
    inputs=[
        "$(S)/tests/vte/color_cases.py",
        "$(S)/tests/vte/color_file_names.txt",
        "$(S)/tests/vte/color_validate.py",
        "$(S)/tests/vte/upstream/color-test.cc",
        "$(S)/tests/vte/upstream/color-names-tests.hh",
    ],
    outputs=["$(B)/tests/vte/color/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/color_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/color/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VC",
    color="cyan",
)


vte_paste_cases = (vte_root / "paste_file_names.txt").read_text().split()
vte_paste_tests = []
for case in vte_paste_cases:
    vte_paste_tests.append(command(
        name="vte_paste_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/paste_adapter.py",
            "$(S)/tests/vte/paste_cases.py",
            "$(S)/tests/vte/paste_file_names.txt",
            "$(S)/tests/vte/upstream/pastify-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/paste/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/paste_adapter.py",
            case,
            f"$(B)/tests/vte/paste/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VP",
        color="cyan",
    ))


vte_paste_validation = command(
    name="vte_paste_catalog",
    inputs=[
        "$(S)/tests/vte/paste_cases.py",
        "$(S)/tests/vte/paste_file_names.txt",
        "$(S)/tests/vte/paste_validate.py",
        "$(S)/tests/vte/upstream/pastify-test.cc",
    ],
    outputs=["$(B)/tests/vte/paste/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/paste_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/paste/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VP",
    color="cyan",
)


vte_utf8_cases = (vte_root / "utf8_file_names.txt").read_text().split()
vte_utf8_tests = []
for case in vte_utf8_cases:
    vte_utf8_tests.append(command(
        name="vte_utf8_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/utf8_adapter.py",
            "$(S)/tests/vte/utf8_cases.py",
            "$(S)/tests/vte/utf8_file_names.txt",
        ],
        outputs=[f"$(B)/tests/vte/utf8/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/utf8_adapter.py",
            case,
            f"$(B)/tests/vte/utf8/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VU",
        color="cyan",
    ))


vte_utf8_validation = command(
    name="vte_utf8_catalog",
    inputs=[
        "$(S)/tests/vte/utf8_cases.py",
        "$(S)/tests/vte/utf8_file_names.txt",
        "$(S)/tests/vte/utf8_validate.py",
    ],
    outputs=["$(B)/tests/vte/utf8/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/utf8_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/utf8/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VU",
    color="cyan",
)


vte_width_cases = (vte_root / "width_file_names.txt").read_text().split()
vte_width_tests = []
for case in vte_width_cases:
    vte_width_tests.append(command(
        name="vte_width_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vte/width_adapter.py",
            "$(S)/tests/vte/width_catalog.py",
            "$(S)/tests/vte/width_file_names.txt",
            "$(S)/tests/vte/width_xfail.txt",
            "$(S)/tests/vte/upstream/unicode-width-test.cc",
        ],
        outputs=[f"$(B)/tests/vte/width/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vte/width_adapter.py",
            case,
            "tests/vte/width_xfail.txt",
            f"$(B)/tests/vte/width/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VW",
        color="cyan",
    ))


vte_width_validation = command(
    name="vte_width_catalog",
    inputs=[
        "$(S)/tests/vte/width_catalog.py",
        "$(S)/tests/vte/width_file_names.txt",
        "$(S)/tests/vte/width_validate.py",
        "$(S)/tests/vte/width_xfail.txt",
        "$(S)/tests/vte/upstream/unicode-width-test.cc",
    ],
    outputs=["$(B)/tests/vte/width/catalog.stamp"],
    cmd=[
        ["python3", "tests/vte/width_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/vte/width/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VW",
    color="cyan",
)


windows_terminal_root = Path(__file__).parent / "tests" / "windows_terminal"
windows_terminal_cases = (windows_terminal_root / "file_names.txt").read_text().split()
windows_terminal_tests = []
for case in windows_terminal_cases:
    windows_terminal_tests.append(command(
        name="windows_terminal_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/windows_terminal/adapter.py",
            "$(S)/tests/windows_terminal/catalog.py",
            "$(S)/tests/windows_terminal/file_names.txt",
            "$(S)/tests/windows_terminal/xfail.txt",
            "$(S)/tests/windows_terminal/upstream/StateMachineTest.cpp",
            "$(S)/tests/windows_terminal/upstream/OutputEngineTest.cpp",
        ],
        outputs=[f"$(B)/tests/windows_terminal/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/windows_terminal/adapter.py",
            case,
            "tests/windows_terminal/xfail.txt",
            f"$(B)/tests/windows_terminal/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WT",
        color="cyan",
    ))


windows_terminal_validation = command(
    name="windows_terminal_catalog",
    inputs=[
        "$(S)/tests/windows_terminal/catalog.py",
        "$(S)/tests/windows_terminal/file_names.txt",
        "$(S)/tests/windows_terminal/validate.py",
        "$(S)/tests/windows_terminal/xfail.txt",
        "$(S)/tests/windows_terminal/upstream/StateMachineTest.cpp",
        "$(S)/tests/windows_terminal/upstream/OutputEngineTest.cpp",
    ],
    outputs=["$(B)/tests/windows_terminal/catalog.stamp"],
    cmd=[
        ["python3", "tests/windows_terminal/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/windows_terminal/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WT",
    color="cyan",
)


wezterm_root = Path(__file__).parent / "tests" / "wezterm"
wezterm_cases = (wezterm_root / "file_names.txt").read_text().split()
wezterm_tests = []
for case in wezterm_cases:
    wezterm_tests.append(command(
        name="wezterm_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/wezterm/adapter.py",
            "$(S)/tests/wezterm/catalog.py",
            "$(S)/tests/wezterm/file_names.txt",
            "$(S)/tests/wezterm/xfail.txt",
            *build.glob("$(S)/tests/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tests/wezterm/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/adapter.py",
            case,
            "tests/wezterm/xfail.txt",
            f"$(B)/tests/wezterm/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WZ",
        color="cyan",
    ))


wezterm_validation = command(
    name="wezterm_catalog",
    inputs=[
        "$(S)/tests/wezterm/catalog.py",
        "$(S)/tests/wezterm/file_names.txt",
        "$(S)/tests/wezterm/validate.py",
        "$(S)/tests/wezterm/xfail.txt",
        *build.glob("$(S)/tests/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tests/wezterm/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WZ",
    color="cyan",
)


wezterm_screen_cases = (
    wezterm_root / "screen_file_names.txt"
).read_text().split()
wezterm_screen_tests = []
for case in wezterm_screen_cases:
    wezterm_screen_tests.append(command(
        name="wezterm_screen_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/catalog.py",
            "$(S)/tests/wezterm/screen_adapter.py",
            "$(S)/tests/wezterm/screen_catalog.py",
            "$(S)/tests/wezterm/screen_file_names.txt",
            "$(S)/tests/wezterm/screen_xfail.txt",
            *build.glob("$(S)/tests/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tests/wezterm/screen/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/screen_adapter.py",
            case,
            "tests/wezterm/screen_xfail.txt",
            f"$(B)/tests/wezterm/screen/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WS",
        color="cyan",
    ))


wezterm_screen_validation = command(
    name="wezterm_screen_catalog",
    inputs=[
        "$(S)/tests/wezterm/catalog.py",
        "$(S)/tests/wezterm/screen_catalog.py",
        "$(S)/tests/wezterm/screen_file_names.txt",
        "$(S)/tests/wezterm/screen_validate.py",
        "$(S)/tests/wezterm/screen_xfail.txt",
        *build.glob("$(S)/tests/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tests/wezterm/screen/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/screen_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/screen/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WS",
    color="cyan",
)


wezterm_selection_cases = (
    wezterm_root / "selection_file_names.txt"
).read_text().split()
wezterm_selection_tests = []
for case in wezterm_selection_cases:
    wezterm_selection_tests.append(command(
        name="wezterm_selection_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/upstream/selection.rs",
            "$(S)/tests/wezterm/selection_adapter.py",
            "$(S)/tests/wezterm/selection_cases.py",
            "$(S)/tests/wezterm/selection_file_names.txt",
        ],
        outputs=[f"$(B)/tests/wezterm/selection/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/selection_adapter.py",
            case,
            f"$(B)/tests/wezterm/selection/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WQ",
        color="cyan",
    ))

wezterm_selection_validation = command(
    name="wezterm_selection_catalog",
    inputs=[
        "$(S)/tests/wezterm/upstream/selection.rs",
        "$(S)/tests/wezterm/selection_cases.py",
        "$(S)/tests/wezterm/selection_file_names.txt",
        "$(S)/tests/wezterm/selection_validate.py",
    ],
    outputs=["$(B)/tests/wezterm/selection/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/selection_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/selection/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WQ",
    color="cyan",
)


wezterm_cursor_cases = (
    wezterm_root / "cursor_file_names.txt"
).read_text().split()
wezterm_cursor_tests = []
for case in wezterm_cursor_cases:
    wezterm_cursor_tests.append(command(
        name="wezterm_cursor_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/cursor_adapter.py",
            "$(S)/tests/wezterm/cursor_cases.py",
            "$(S)/tests/wezterm/cursor_file_names.txt",
            "$(S)/tests/wezterm/screen_catalog.py",
            *build.glob("$(S)/tests/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tests/wezterm/cursor/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/cursor_adapter.py",
            case,
            f"$(B)/tests/wezterm/cursor/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WC",
        color="cyan",
    ))

wezterm_cursor_validation = command(
    name="wezterm_cursor_catalog",
    inputs=[
        "$(S)/tests/wezterm/cursor_cases.py",
        "$(S)/tests/wezterm/cursor_file_names.txt",
        "$(S)/tests/wezterm/cursor_validate.py",
        "$(S)/tests/wezterm/screen_catalog.py",
        *build.glob("$(S)/tests/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tests/wezterm/cursor/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/cursor_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/cursor/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WC",
    color="cyan",
)


wezterm_damage_cases = (
    wezterm_root / "damage_file_names.txt"
).read_text().split()
wezterm_damage_tests = []
for case in wezterm_damage_cases:
    wezterm_damage_tests.append(command(
        name="wezterm_damage_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/damage_adapter.py",
            "$(S)/tests/wezterm/damage_cases.py",
            "$(S)/tests/wezterm/damage_file_names.txt",
            "$(S)/tests/wezterm/screen_catalog.py",
            "$(S)/tests/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tests/wezterm/damage/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/damage_adapter.py",
            case,
            f"$(B)/tests/wezterm/damage/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WD",
        color="cyan",
    ))

wezterm_damage_validation = command(
    name="wezterm_damage_catalog",
    inputs=[
        "$(S)/tests/wezterm/damage_cases.py",
        "$(S)/tests/wezterm/damage_file_names.txt",
        "$(S)/tests/wezterm/damage_validate.py",
        "$(S)/tests/wezterm/screen_catalog.py",
        "$(S)/tests/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tests/wezterm/damage/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/damage_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/damage/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WD",
    color="cyan",
)


wezterm_history_cases = (
    wezterm_root / "history_file_names.txt"
).read_text().split()
wezterm_history_tests = []
for case in wezterm_history_cases:
    wezterm_history_tests.append(command(
        name="wezterm_history_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/catalog.py",
            "$(S)/tests/wezterm/history_adapter.py",
            "$(S)/tests/wezterm/history_cases.py",
            "$(S)/tests/wezterm/history_file_names.txt",
            "$(S)/tests/wezterm/screen_catalog.py",
            "$(S)/tests/wezterm/upstream/csi.rs",
            "$(S)/tests/wezterm/upstream/mod.rs",
            "$(S)/tests/wezterm/upstream/selection.rs",
        ],
        outputs=[f"$(B)/tests/wezterm/history/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/history_adapter.py",
            case,
            f"$(B)/tests/wezterm/history/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WH",
        color="cyan",
    ))

wezterm_history_validation = command(
    name="wezterm_history_catalog",
    inputs=[
        "$(S)/tests/wezterm/catalog.py",
        "$(S)/tests/wezterm/history_cases.py",
        "$(S)/tests/wezterm/history_file_names.txt",
        "$(S)/tests/wezterm/history_validate.py",
        "$(S)/tests/wezterm/screen_catalog.py",
        "$(S)/tests/wezterm/upstream/csi.rs",
        "$(S)/tests/wezterm/upstream/mod.rs",
        "$(S)/tests/wezterm/upstream/selection.rs",
    ],
    outputs=["$(B)/tests/wezterm/history/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/history_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/history/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WH",
    color="cyan",
)


wezterm_semantic_cases = (
    wezterm_root / "semantic_file_names.txt"
).read_text().split()
wezterm_semantic_tests = []
for case in wezterm_semantic_cases:
    wezterm_semantic_tests.append(command(
        name="wezterm_semantic_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/semantic_adapter.py",
            "$(S)/tests/wezterm/semantic_cases.py",
            "$(S)/tests/wezterm/semantic_file_names.txt",
            "$(S)/tests/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tests/wezterm/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/semantic_adapter.py",
            case,
            f"$(B)/tests/wezterm/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WM",
        color="cyan",
    ))

wezterm_semantic_validation = command(
    name="wezterm_semantic_catalog",
    inputs=[
        "$(S)/tests/wezterm/semantic_cases.py",
        "$(S)/tests/wezterm/semantic_file_names.txt",
        "$(S)/tests/wezterm/semantic_validate.py",
        "$(S)/tests/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tests/wezterm/semantic/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WM",
    color="cyan",
)


wezterm_hyperlink_cases = (
    wezterm_root / "hyperlink_file_names.txt"
).read_text().split()
wezterm_hyperlink_tests = []
for case in wezterm_hyperlink_cases:
    wezterm_hyperlink_tests.append(command(
        name="wezterm_hyperlink_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/hyperlink_adapter.py",
            "$(S)/tests/wezterm/hyperlink_cases.py",
            "$(S)/tests/wezterm/hyperlink_file_names.txt",
            "$(S)/tests/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tests/wezterm/hyperlink/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/hyperlink_adapter.py",
            case,
            f"$(B)/tests/wezterm/hyperlink/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WL",
        color="cyan",
    ))

wezterm_hyperlink_validation = command(
    name="wezterm_hyperlink_catalog",
    inputs=[
        "$(S)/tests/wezterm/hyperlink_cases.py",
        "$(S)/tests/wezterm/hyperlink_file_names.txt",
        "$(S)/tests/wezterm/hyperlink_validate.py",
        "$(S)/tests/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tests/wezterm/hyperlink/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/hyperlink_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/hyperlink/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WL",
    color="cyan",
)


wezterm_metadata_cases = (
    wezterm_root / "metadata_file_names.txt"
).read_text().split()
wezterm_metadata_tests = []
for case in wezterm_metadata_cases:
    wezterm_metadata_tests.append(command(
        name="wezterm_metadata_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wezterm/metadata_adapter.py",
            "$(S)/tests/wezterm/metadata_cases.py",
            "$(S)/tests/wezterm/metadata_file_names.txt",
            "$(S)/tests/wezterm/upstream/csi.rs",
            "$(S)/tests/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tests/wezterm/metadata/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/wezterm/metadata_adapter.py",
            case,
            f"$(B)/tests/wezterm/metadata/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WX",
        color="cyan",
    ))

wezterm_metadata_validation = command(
    name="wezterm_metadata_catalog",
    inputs=[
        "$(S)/tests/wezterm/metadata_cases.py",
        "$(S)/tests/wezterm/metadata_file_names.txt",
        "$(S)/tests/wezterm/metadata_validate.py",
        "$(S)/tests/wezterm/upstream/csi.rs",
        "$(S)/tests/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tests/wezterm/metadata/catalog.stamp"],
    cmd=[
        ["python3", "tests/wezterm/metadata_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/wezterm/metadata/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WX",
    color="cyan",
)


konsole_root = Path(__file__).parent / "tests" / "konsole"
konsole_cases = (konsole_root / "file_names.txt").read_text().split()
konsole_tests = []
for case in konsole_cases:
    konsole_tests.append(command(
        name="konsole_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/konsole/adapter.py",
            "$(S)/tests/konsole/catalog.py",
            "$(S)/tests/konsole/file_names.txt",
            "$(S)/tests/konsole/xfail.txt",
            "$(S)/tests/konsole/upstream/Vt102EmulationTest.cpp",
        ],
        outputs=[f"$(B)/tests/konsole/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/konsole/adapter.py",
            case,
            "tests/konsole/xfail.txt",
            f"$(B)/tests/konsole/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KO",
        color="cyan",
    ))


konsole_validation = command(
    name="konsole_catalog",
    inputs=[
        "$(S)/tests/konsole/catalog.py",
        "$(S)/tests/konsole/file_names.txt",
        "$(S)/tests/konsole/validate.py",
        "$(S)/tests/konsole/xfail.txt",
        "$(S)/tests/konsole/upstream/Vt102EmulationTest.cpp",
    ],
    outputs=["$(B)/tests/konsole/catalog.stamp"],
    cmd=[
        ["python3", "tests/konsole/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/konsole/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KO",
    color="cyan",
)


konsole_semantic_cases = (
    konsole_root / "semantic_file_names.txt"
).read_text().split()
konsole_semantic_tests = []
for case in konsole_semantic_cases:
    konsole_semantic_tests.append(command(
        name="konsole_semantic_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/konsole/semantic_adapter.py",
            "$(S)/tests/konsole/semantic_cases.py",
            "$(S)/tests/konsole/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tests/konsole/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/konsole/semantic_adapter.py",
            case,
            f"$(B)/tests/konsole/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KS",
        color="cyan",
    ))

konsole_semantic_validation = command(
    name="konsole_semantic_catalog",
    inputs=[
        "$(S)/tests/konsole/semantic_cases.py",
        "$(S)/tests/konsole/semantic_file_names.txt",
        "$(S)/tests/konsole/semantic_validate.py",
    ],
    outputs=["$(B)/tests/konsole/semantic/catalog.stamp"],
    cmd=[
        ["python3", "tests/konsole/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/konsole/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KS",
    color="cyan",
)


konsole_vt_cases = (
    konsole_root / "vt_file_names.txt"
).read_text().split()
konsole_vt_tests = []
for case in konsole_vt_cases:
    konsole_vt_tests.append(command(
        name="konsole_vt_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/konsole/vt_adapter.py",
            "$(S)/tests/konsole/vt_cases.py",
            "$(S)/tests/konsole/vt_file_names.txt",
        ],
        outputs=[f"$(B)/tests/konsole/vt/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/konsole/vt_adapter.py",
            case,
            f"$(B)/tests/konsole/vt/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KV",
        color="cyan",
    ))

konsole_vt_validation = command(
    name="konsole_vt_catalog",
    inputs=[
        "$(S)/tests/konsole/vt_cases.py",
        "$(S)/tests/konsole/vt_file_names.txt",
        "$(S)/tests/konsole/vt_validate.py",
    ],
    outputs=["$(B)/tests/konsole/vt/catalog.stamp"],
    cmd=[
        ["python3", "tests/konsole/vt_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/konsole/vt/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KV",
    color="cyan",
)


konsole_width_cases = (
    konsole_root / "width_file_names.txt"
).read_text().split()
konsole_width_tests = []
for case in konsole_width_cases:
    konsole_width_tests.append(command(
        name="konsole_width_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/konsole/upstream/CharacterWidthTest.cpp",
            "$(S)/tests/konsole/width_adapter.py",
            "$(S)/tests/konsole/width_catalog.py",
            "$(S)/tests/konsole/width_file_names.txt",
        ],
        outputs=[f"$(B)/tests/konsole/width/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/konsole/width_adapter.py",
            case,
            f"$(B)/tests/konsole/width/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KW",
        color="cyan",
    ))

konsole_width_validation = command(
    name="konsole_width_catalog",
    inputs=[
        "$(S)/tests/konsole/upstream/CharacterWidthTest.cpp",
        "$(S)/tests/konsole/width_catalog.py",
        "$(S)/tests/konsole/width_file_names.txt",
        "$(S)/tests/konsole/width_validate.py",
    ],
    outputs=["$(B)/tests/konsole/width/catalog.stamp"],
    cmd=[
        ["python3", "tests/konsole/width_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/konsole/width/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KW",
    color="cyan",
)


konsole_keyboard_cases = (
    konsole_root / "keyboard_file_names.txt"
).read_text().split()
konsole_keyboard_tests = []
for case in konsole_keyboard_cases:
    konsole_keyboard_tests.append(command(
        name="konsole_keyboard_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/konsole/upstream/KeyboardTranslatorTest.cpp",
            "$(S)/tests/konsole/keyboard_adapter.py",
            "$(S)/tests/konsole/keyboard_catalog.py",
            "$(S)/tests/konsole/keyboard_file_names.txt",
        ],
        outputs=[f"$(B)/tests/konsole/keyboard/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/konsole/keyboard_adapter.py",
            case,
            f"$(B)/tests/konsole/keyboard/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KK",
        color="cyan",
    ))

konsole_keyboard_validation = command(
    name="konsole_keyboard_catalog",
    inputs=[
        "$(S)/tests/konsole/upstream/KeyboardTranslatorTest.cpp",
        "$(S)/tests/konsole/keyboard_catalog.py",
        "$(S)/tests/konsole/keyboard_file_names.txt",
        "$(S)/tests/konsole/keyboard_validate.py",
    ],
    outputs=["$(B)/tests/konsole/keyboard/catalog.stamp"],
    cmd=[
        ["python3", "tests/konsole/keyboard_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/konsole/keyboard/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KK",
    color="cyan",
)


konsole_pty_cases = (
    konsole_root / "pty_file_names.txt"
).read_text().split()
konsole_pty_tests = []
for case in konsole_pty_cases:
    konsole_pty_tests.append(command(
        name="konsole_pty_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/konsole/upstream/PtyTest.cpp",
            "$(S)/tests/konsole/pty_adapter.py",
            "$(S)/tests/konsole/pty_catalog.py",
            "$(S)/tests/konsole/pty_file_names.txt",
        ],
        outputs=[f"$(B)/tests/konsole/pty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/konsole/pty_adapter.py",
            case,
            f"$(B)/tests/konsole/pty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KP",
        color="cyan",
    ))

konsole_pty_validation = command(
    name="konsole_pty_catalog",
    inputs=[
        "$(S)/tests/konsole/upstream/PtyTest.cpp",
        "$(S)/tests/konsole/pty_catalog.py",
        "$(S)/tests/konsole/pty_file_names.txt",
        "$(S)/tests/konsole/pty_validate.py",
    ],
    outputs=["$(B)/tests/konsole/pty/catalog.stamp"],
    cmd=[
        ["python3", "tests/konsole/pty_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/konsole/pty/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KP",
    color="cyan",
)


tmux_root = Path(__file__).parent / "tests" / "tmux"
tmux_corpus_members = [
    "corpus/" + path.name
    for path in sorted((tmux_root / "corpus").iterdir())
]
tmux_dictionary_members = [
    f"dictionary/{index:03d}"
    for index, line in enumerate(
        (tmux_root / "upstream" / "input-fuzzer.dict").read_text().splitlines()
    )
    if line.strip() and not line.startswith("#")
]
tmux_members = tmux_corpus_members + tmux_dictionary_members
tmux_xfails = {
    line.strip()
    for line in (tmux_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_tmux_xfails = tmux_xfails - set(tmux_members)
if unknown_tmux_xfails:
    raise RuntimeError(
        "unknown tmux XFAIL members: "
        + ", ".join(sorted(unknown_tmux_xfails))
    )

tmux_tests = []
tmux_shard_size = 128
for shard_index, start in enumerate(
    range(0, len(tmux_corpus_members), tmux_shard_size)
):
    shard = tmux_corpus_members[start : start + tmux_shard_size]
    name = f"input_corpus_{shard_index:03d}"
    tmux_tests.append(command(
        name="tmux_" + name,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/tmux/adapter.py",
            "$(S)/tests/tmux/xfail.txt",
            *("$(S)/tests/tmux/" + member for member in shard),
        ],
        outputs=[f"$(B)/tests/tmux/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/tmux/adapter.py",
            "tests/tmux/xfail.txt",
            f"$(B)/tests/tmux/{name}.stamp",
            *shard,
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TM",
        color="cyan",
    ))

for member in tmux_dictionary_members:
    index = member.split("/", 1)[1]
    tmux_tests.append(command(
        name="tmux_input_dictionary_" + index,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/fuzz_parser.py",
            "$(S)/tests/tmux/adapter.py",
            "$(S)/tests/tmux/xfail.txt",
            "$(S)/tests/tmux/upstream/input-fuzzer.dict",
        ],
        outputs=[f"$(B)/tests/tmux/input_dictionary_{index}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/tmux/adapter.py",
            "tests/tmux/xfail.txt",
            f"$(B)/tests/tmux/input_dictionary_{index}.stamp",
            member,
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TM",
        color="cyan",
    ))


wraptest_helper = program(
    name="wraptest_helper",
    srcs=["$(S)/tests/wraptest/wraptest.c"],
    cflags=["-Wno-error"],
    output="$(B)/tests/wraptest/wraptest",
)

wraptest_root = Path(__file__).parent / "tests" / "wraptest"
wraptest_cases = json.loads((wraptest_root / "cases.json").read_text())
wraptest_tests = []
for case_id, _, _ in wraptest_cases:
    wraptest_tests.append(command(
        name="wraptest_" + case_id,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/wraptest/adapter.py",
            "$(S)/tests/wraptest/cases.json",
            "$(S)/tests/wraptest/xfail.txt",
        ],
        outputs=[f"$(B)/tests/wraptest/{case_id}.stamp"],
        deps=[st_test, wraptest_helper],
        cmd=[
            "python3",
            "tests/wraptest/adapter.py",
            "$(B)/tests/wraptest/wraptest",
            case_id,
            "tests/wraptest/xfail.txt",
            f"$(B)/tests/wraptest/{case_id}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WR",
        color="cyan",
    ))


tack_root = Path(__file__).parent / "tests" / "tack"
tack_cases = (tack_root / "file_names.txt").read_text().split()
tack_xfails = {
    line.strip()
    for line in (tack_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_tack_xfails = tack_xfails - set(tack_cases)
if unknown_tack_xfails:
    raise RuntimeError(
        "unknown tack XFAIL capabilities: "
        + ", ".join(sorted(unknown_tack_xfails))
    )
tack_upstream_inputs = build.glob("$(S)/tests/tack/upstream/*")
tack_program = command(
    name="tack_program",
    inputs=[
        "$(S)/tests/tack/build_tack.sh",
        *tack_upstream_inputs,
    ],
    outputs=["$(B)/tests/tack/tack"],
    cmd=[
        "sh",
        "tests/tack/build_tack.sh",
        "tests/tack/upstream",
        "$(B)/tests/tack/tack",
    ],
    cwd="$(S)",
    descr="TC",
    color="magenta",
)
tack_validation = command(
    name="tack_catalog",
    inputs=[
        "$(S)/tests/tack/file_names.txt",
        "$(S)/tests/tack/validate.py",
        *tack_upstream_inputs,
    ],
    outputs=["$(B)/tests/tack/catalog.stamp"],
    cmd=[
        ["python3", "tests/tack/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/tack/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="TA",
    color="cyan",
)
tack_tests = []
for capability in tack_cases:
    tack_tests.append(command(
        name="tack_" + capability,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/tack/adapter.py",
            "$(S)/tests/tack/file_names.txt",
            "$(S)/tests/tack/xfail.txt",
        ],
        outputs=[f"$(B)/tests/tack/{capability}.stamp"],
        deps=[st_test, tack_program],
        cmd=[
            "python3",
            "tests/tack/adapter.py",
            "$(B)/tests/tack/tack",
            capability,
            "tests/tack/xfail.txt",
            f"$(B)/tests/tack/{capability}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TA",
        color="cyan",
    ))


ucs_detect_root = Path(__file__).parent / "tests" / "ucs_detect"
ucs_detect_tests = []
ucs_detect_table_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted(ucs_detect_root.glob("table_*.py"))
]
ucs_detect_shards = [
    (category, int(start), int(end))
    for category, start, end in (
        line.split() for line in
        (ucs_detect_root / "shards.txt").read_text().splitlines()
    )
]
ucs_detect_category_indices = {}
for category, start, end in ucs_detect_shards:
    shard_index = ucs_detect_category_indices.get(category, 0)
    ucs_detect_category_indices[category] = shard_index + 1
    name = f"{category}_{shard_index:03d}"
    ucs_detect_tests.append(command(
        name="ucs_detect_" + name,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/ucs_detect/adapter.py",
            "$(S)/tests/ucs_detect/catalog.py",
            "$(S)/tests/ucs_detect/shards.txt",
            "$(S)/tests/ucs_detect/xfail.txt",
            *ucs_detect_table_inputs,
        ],
        outputs=[f"$(B)/tests/ucs_detect/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/ucs_detect/adapter.py",
            category,
            str(start),
            str(end),
            "tests/ucs_detect/xfail.txt",
            f"$(B)/tests/ucs_detect/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="UC",
        color="cyan",
    ))

ucs_detect_validation = command(
    name="ucs_detect_catalog",
    inputs=[
        "$(S)/tests/ucs_detect/catalog.py",
        "$(S)/tests/ucs_detect/probe_cases.py",
        "$(S)/tests/ucs_detect/probe_names.txt",
        "$(S)/tests/ucs_detect/probe_xfail.txt",
        "$(S)/tests/ucs_detect/validate.py",
        "$(S)/tests/ucs_detect/shards.txt",
        "$(S)/tests/ucs_detect/xfail.txt",
        *ucs_detect_table_inputs,
    ],
    outputs=["$(B)/tests/ucs_detect/catalog.stamp"],
    cmd=[
        ["python3", "tests/ucs_detect/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/ucs_detect/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="UC",
    color="cyan",
)


ucs_detect_probe_cases = (ucs_detect_root / "probe_names.txt").read_text().split()
ucs_detect_probe_tests = []
for case in ucs_detect_probe_cases:
    ucs_detect_probe_tests.append(command(
        name="ucs_detect_probe_" + case.lower(),
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/ucs_detect/probe_adapter.py",
            "$(S)/tests/ucs_detect/probe_cases.py",
            "$(S)/tests/ucs_detect/probe_names.txt",
            "$(S)/tests/ucs_detect/probe_xfail.txt",
            "$(S)/tests/ucs_detect/upstream/terminal.py",
            "$(S)/tests/ucs_detect/upstream/table_xtgettcap.py",
            "$(S)/tests/ucs_detect/upstream/data/shitty.yaml",
        ],
        outputs=[f"$(B)/tests/ucs_detect/probes/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/ucs_detect/probe_adapter.py",
            case,
            "tests/ucs_detect/probe_xfail.txt",
            f"$(B)/tests/ucs_detect/probes/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="UP",
        color="cyan",
    ))


vtebench_root = Path(__file__).parent / "tests" / "vtebench"
vtebench_cases = (vtebench_root / "file_names.txt").read_text().split()
vtebench_tests = []
for case in vtebench_cases:
    vtebench_case_inputs = [
        "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
        for path in sorted((vtebench_root / "benchmarks" / case).iterdir())
        if path.is_file()
    ]
    vtebench_tests.append(command(
        name="vtebench_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/vtebench/adapter.py",
            "$(S)/tests/vtebench/file_names.txt",
            "$(S)/tests/vtebench/xfail.txt",
            *vtebench_case_inputs,
        ],
        outputs=[f"$(B)/tests/vtebench/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/vtebench/adapter.py",
            case,
            "tests/vtebench/xfail.txt",
            f"$(B)/tests/vtebench/{case}.stamp",
            "30",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VB",
        color="yellow",
    ))


libvterm_root = Path(__file__).parent / "tests" / "libvterm"
libvterm_cases = (libvterm_root / "file_names.txt").read_text().split()
libvterm_xfails = {
    line.strip()
    for line in (libvterm_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_libvterm_xfails = libvterm_xfails - set(libvterm_cases)
if unknown_libvterm_xfails:
    raise RuntimeError(
        "unknown libvterm XFAIL fixtures: "
        + ", ".join(sorted(unknown_libvterm_xfails))
    )
libvterm_tests = []
for case in libvterm_cases:
    name = case.removesuffix(".test").replace("-", "_")
    libvterm_tests.append(command(
        name="libvterm_" + name,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/libvterm/adapter.py",
            "$(S)/tests/libvterm/file_names.txt",
            "$(S)/tests/libvterm/xfail.txt",
            f"$(S)/tests/libvterm/upstream/{case}",
        ],
        outputs=[f"$(B)/tests/libvterm/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/libvterm/adapter.py",
            case,
            "tests/libvterm/xfail.txt",
            f"$(B)/tests/libvterm/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="LV",
        color="cyan",
    ))


xterm_vttests_root = Path(__file__).parent / "tests" / "xterm_vttests"
xterm_vttests_cases = (xterm_vttests_root / "file_names.txt").read_text().split()
xterm_vttests_xfails = {
    line.strip()
    for line in (xterm_vttests_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_xterm_vttests_xfails = xterm_vttests_xfails - set(xterm_vttests_cases)
if unknown_xterm_vttests_xfails:
    raise RuntimeError(
        "unknown xterm vttests XFAIL scripts: "
        + ", ".join(sorted(unknown_xterm_vttests_xfails))
    )
xterm_vttests_tests = []
for case in xterm_vttests_cases:
    name = case.removesuffix(".sh").replace("-", "_")
    xterm_vttests_tests.append(command(
        name="xterm_vttests_" + name,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/xterm_vttests/adapter.py",
            "$(S)/tests/xterm_vttests/file_names.txt",
            "$(S)/tests/xterm_vttests/xfail.txt",
            *build.glob("$(S)/tests/xterm_vttests/bin/*"),
            *build.glob("$(S)/tests/xterm_vttests/lib/**/*.pm"),
            f"$(S)/tests/xterm_vttests/upstream/{case}",
        ],
        outputs=[f"$(B)/tests/xterm_vttests/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/xterm_vttests/adapter.py",
            case,
            "tests/xterm_vttests/xfail.txt",
            f"$(B)/tests/xterm_vttests/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="XV",
        color="cyan",
    ))


esctest_root = Path(__file__).parent / "tests" / "esctest"
esctest_cases = (esctest_root / "file_names.txt").read_text().split()
esctest_xfails = {
    line.strip()
    for line in (esctest_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_esctest_xfails = esctest_xfails - set(esctest_cases)
if unknown_esctest_xfails:
    raise RuntimeError(
        "unknown esctest XFAIL cases: "
        + ", ".join(sorted(unknown_esctest_xfails))
    )
esctest_ported_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted((esctest_root / "ported").rglob("*.py"))
]
esctest_tests = []
for case in esctest_cases:
    name = case.replace(".", "_")
    esctest_tests.append(command(
        name="esctest_" + name,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/esctest/adapter.py",
            "$(S)/tests/esctest/file_names.txt",
            "$(S)/tests/esctest/xfail.txt",
            *esctest_ported_inputs,
        ],
        outputs=[f"$(B)/tests/esctest/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/esctest/adapter.py",
            case,
            "tests/esctest/xfail.txt",
            f"$(B)/tests/esctest/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="ES",
        color="cyan",
    ))


termless_root = Path(__file__).parent / "tests" / "termless"
termless_cases = json.loads((termless_root / "cases.json").read_text())
termless_ids = {case_id for case_id, _, _ in termless_cases}
termless_xfails = {
    line.strip()
    for line in (termless_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_termless_xfails = termless_xfails - termless_ids
if unknown_termless_xfails:
    raise RuntimeError(
        "unknown Termless XFAIL cases: "
        + ", ".join(sorted(unknown_termless_xfails))
    )
termless_upstream_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted((termless_root / "upstream").rglob("*"))
    if path.is_file()
]
termless_validation = command(
    name="termless_catalog",
    inputs=[
        "$(S)/tests/termless/cases.json",
        "$(S)/tests/termless/validate.py",
        "$(S)/tests/termless/xfail.txt",
        *termless_upstream_inputs,
    ],
    outputs=["$(B)/tests/termless/catalog.stamp"],
    cmd=[
        ["python3", "tests/termless/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/termless/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="TL",
    color="cyan",
)
termless_tests = []
for case_id, _, _ in termless_cases:
    termless_tests.append(command(
        name="termless_" + case_id,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/termless/adapter.py",
            "$(S)/tests/termless/backend.py",
            "$(S)/tests/termless/cases.py",
            "$(S)/tests/termless/cases.json",
            "$(S)/tests/termless/xfail.txt",
            *termless_upstream_inputs,
        ],
        outputs=[f"$(B)/tests/termless/{case_id}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/termless/adapter.py",
            case_id,
            "tests/termless/xfail.txt",
            f"$(B)/tests/termless/{case_id}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TL",
        color="cyan",
    ))


realworld_root = Path(__file__).parent / "tests" / "realworld"
realworld_cases = (realworld_root / "file_names.txt").read_text().split()
realworld_validation = command(
    name="realworld_catalog",
    inputs=[
        "$(S)/tests/realworld/validate.py",
        "$(S)/tests/realworld/corpus.py",
        "$(S)/tests/realworld/cases.json",
        "$(S)/tests/realworld/file_names.txt",
        *build.glob("$(S)/tests/realworld/input/*.input.zst"),
        *build.glob("$(S)/tests/realworld/screen/*.screen.json"),
    ],
    outputs=["$(B)/tests/realworld/catalog.stamp"],
    cmd=[
        ["python3", "tests/realworld/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tests/realworld/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="RW",
    color="cyan",
)
realworld_tests = []
for case in realworld_cases:
    realworld_tests.append(command(
        name="realworld_" + case,
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/realworld/adapter.py",
            "$(S)/tests/realworld/corpus.py",
            "$(S)/tests/realworld/cases.json",
            "$(S)/tests/realworld/file_names.txt",
            f"$(S)/tests/realworld/input/{case}.input.zst",
            f"$(S)/tests/realworld/screen/{case}.screen.json",
        ],
        outputs=[f"$(B)/tests/realworld/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tests/realworld/adapter.py",
            case,
            f"$(B)/tests/realworld/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="RW",
        color="cyan",
    ))

# The Cartesian key-encoding matrix is ~6500 cases; ten shards keep
# each node inside the ordinary test timeout, split by a stable hash
# of the case id.
keyboard_product_group_count = 10
keyboard_product_tests = []
for group_index in range(keyboard_product_group_count):
    output = f"$(B)/keyboard-product/group-{group_index:02}.stamp"
    keyboard_product_tests.append(command(
        name=f"keyboard_product_group_{group_index:02}",
        inputs=[
            "$(S)/tests/harness.py",
            "$(S)/tests/keyboard_layout_product.py",
        ],
        outputs=[output],
        deps=[st_test],
        cmd=[
            [
                "python3",
                "tests/keyboard_layout_product.py",
                f"--group={group_index}",
                f"--group-count={keyboard_product_group_count}",
            ],
            touch_stamp(output),
        ],
        cwd="$(S)",
        env={
            "SHITTY_TEST_BINARY": "$(B)/st_test",
            "SHITTY_TEST_FONTCONFIG": "1" if fontconfig else "0",
            "SHITTY_TEST_PLATFORM": "cocoa" if darwin else "wayland",
            "SHITTY_TEST_VERSION": shitty_version,
        },
        descr="KB",
        color="cyan",
    ))


group("install", st)

add_test(
    *([plt_tests] if plt_tests is not None else []),
    *unit_test_groups,
    *python_test_groups,
    *python_test_prod_parser_groups,
    parser_fuzz,
    vttest_profile,
    *xtermjs_tests,
    *alacritty_tests,
    contour_vttest,
    *contour_tests,
    *mosh_tests,
    *mosh_semantic_tests,
    mosh_semantic_validation,
    *libtsm_semantic_tests,
    libtsm_semantic_validation,
    *ghostty_tests,
    *ghostty_semantic_tests,
    ghostty_semantic_validation,
    *kitty_tests,
    kitty_validation,
    *kitty_screen_tests,
    kitty_screen_validation,
    kitty_utf8,
    *kitty_transaction_tests,
    kitty_transaction_validation,
    *vte_tests,
    vte_validation,
    *vte_known_tests,
    vte_known_validation,
    *vte_charset_tests,
    vte_charset_validation,
    *vte_tabstop_tests,
    vte_tabstop_validation,
    *vte_mode_tests,
    vte_mode_validation,
    *vte_color_tests,
    vte_color_validation,
    *vte_paste_tests,
    vte_paste_validation,
    *vte_utf8_tests,
    vte_utf8_validation,
    *vte_width_tests,
    vte_width_validation,
    *windows_terminal_tests,
    windows_terminal_validation,
    *wezterm_tests,
    wezterm_validation,
    *wezterm_screen_tests,
    wezterm_screen_validation,
    *wezterm_selection_tests,
    wezterm_selection_validation,
    *wezterm_cursor_tests,
    wezterm_cursor_validation,
    *wezterm_damage_tests,
    wezterm_damage_validation,
    *wezterm_history_tests,
    wezterm_history_validation,
    *wezterm_semantic_tests,
    wezterm_semantic_validation,
    *wezterm_hyperlink_tests,
    wezterm_hyperlink_validation,
    *wezterm_metadata_tests,
    wezterm_metadata_validation,
    *konsole_tests,
    konsole_validation,
    *konsole_semantic_tests,
    konsole_semantic_validation,
    *konsole_vt_tests,
    konsole_vt_validation,
    *konsole_width_tests,
    konsole_width_validation,
    *konsole_keyboard_tests,
    konsole_keyboard_validation,
    *konsole_pty_tests,
    konsole_pty_validation,
    *tmux_tests,
    wraptest_helper,
    *wraptest_tests,
    tack_program,
    tack_validation,
    *tack_tests,
    *ucs_detect_tests,
    ucs_detect_validation,
    *ucs_detect_probe_tests,
    *vtebench_tests,
    *libvterm_tests,
    *xterm_vttests_tests,
    *esctest_tests,
    termless_validation,
    *termless_tests,
    realworld_validation,
    *realworld_tests,
    *keyboard_product_tests,
)
