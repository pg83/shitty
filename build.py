import hashlib
import json
import os
import shutil
import subprocess
from datetime import date
from pathlib import Path

import build


std_build = os.path.join("ext", "libstd", "build.py")
plt_build = os.path.join("ext", "plt", "build.py")
shitty_version = date.today().strftime("%Y.%m.%d")

build.flags.allow({
    "group": {
        "descr": "zero-based test partition to include",
        "default": "",
    },
    "group_count": {
        "descr": "total number of test partitions",
        "default": "",
    },
    "coverage": {
        "descr": "instrument for llvm source-based coverage",
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


def add_test(*targets, instrumented=True):
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
        if instrumented:
            group("instrumented-test", target)


# $(S) serves the full-path form cross-library includes use:
# lib/shitty reaches the VT core as <lib/vterm/...>.
build.includes += ["$(B)", "$(S)", "$(S)/lib/shitty", "$(S)/ext"]
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
    "-std=c++26",
    "-Og" if "-DDEBUG" in build.cppflags else "-O2",
]
# -Dcoverage instruments what this graph compiles for llvm source-based
# coverage, so a review wave can bring numbers instead of arguments about
# what the suite reaches. The imported graphs (libstd, plt) stay out of
# it, so the numbers cover lib/shitty, bin and tst:
#
#     ./build -Dcoverage unit_tests
#     LLVM_PROFILE_FILE=/tmp/u.profraw ./.build/unit_tests --threads=1
#     llvm-profdata merge -sparse /tmp/u.profraw -o /tmp/u.profdata
#     llvm-cov report ./.build/unit_tests -instr-profile=/tmp/u.profdata
#
# Adding nothing when the flag is absent is the point: an ordinary build
# stays the ordinary build, down to the bytes of its binaries.
if build.flags.coverage or os.environ.get("SHITTY_COVERAGE"):
    # The -D alone would instrument everything except the one binary worth
    # measuring. Any -D makes the target configuration differ from the host
    # one, and the runner then re-invokes this file for the host with no
    # defines at all; every program the build itself runs - unit_tests among
    # them - is taken from that second graph. The environment is what the
    # re-invocation inherits, so the switch travels in it. Set here rather
    # than lower down on purpose: the imported graphs (libstd, plt) are
    # loaded further down and read no such variable, so they stay
    # uninstrumented - and they have to, an all-instrumented binary dies in
    # the profile runtime before the first test (which is what R2-test saw).
    os.environ["SHITTY_COVERAGE"] = "1"
    build.cxxflags += ["-fprofile-instr-generate", "-fcoverage-mapping"]
    # The profile runtime that writes the .profraw lives on the linker's
    # side of the flag, and every binary this graph links needs it.
    build.ldflags += ["-fprofile-instr-generate"]
production_path_flags = [
    "-ffile-prefix-map=$(S)/lib/vterm=lib/vterm",
    "-ffile-prefix-map=$(S)/lib/embed=lib/embed",
    "-ffile-prefix-map=$(S)/lib/shitty=lib",
    "-ffile-prefix-map=$(S)/bin=bin",
    "-ffile-prefix-map=$(S)/ext=ext",
    "-ffile-prefix-map=$(S)/tst=tst",
    "-ffile-prefix-map=$(B)=.",
]


darwin = "apple-darwin" in build.target
linux = "linux" in build.target


untimed_command = command

def command(**kwargs):
    # Hard per-invocation timeout so one hung test cannot wedge the whole CI
    # run. Only test invocations are wrapped: build steps (ragel, shaders,
    # helper binaries) run untimed.
    test_timeout_seconds = kwargs.pop("test_timeout_seconds", 60)

    def is_test(argv):
        if argv[0] == "$(B)/unit_tests":
            return True
        return argv[0] == "python3" and len(argv) > 1 and (
            argv[1].startswith("tst/") or argv[1] == "-m"
        )

    cmd = kwargs.get("cmd")
    if cmd:
        nested = cmd if isinstance(cmd[0], list) else [cmd]
        if any(is_test(argv) for argv in nested):
            kwargs["cmd"] = [
                [
                    "python3",
                    "$(S)/tst/run_timed.py",
                    str(test_timeout_seconds),
                    *argv,
                ]
                if is_test(argv) else argv
                for argv in nested
            ]
            kwargs["inputs"] = [*kwargs.get("inputs", []), "$(S)/tst/run_timed.py"]
    return untimed_command(**kwargs)

# ponytail: on macOS the CoreText/Metal backend covers the four font packages,
# and linking Homebrew dylibs only makes the built binary die on the next `brew
# upgrade` that bumps a soname. This is what dev/build_brew_macos.sh already did
# with an empty PKG_CONFIG_LIBDIR; 62cef373 made it the default instead. simdutf
# is not a font package, but it is a Homebrew dylib on the same terms, and
# base64.cpp has a portable path to fall back to.
#
# But "darwin" is not the property that reasoning is about. The property is
# where the library lives: a Nix store path is immutable and no upgrade can
# ever move it out from under a linked binary, while /opt/homebrew is rewritten
# in place. Asking `darwin` answered the same for both and switched FreeType,
# fontconfig and harfbuzz off inside the Nix CI too, where flake.nix puts them
# in buildInputs on purpose - font_freetype.cpp was compiled zero times there
# and eleven font tests failed for it (G13).
#
# So ask the paths pkg-config actually hands the compiler and the linker. A
# package survives on darwin only when everything it contributes lives in the
# store, which is exactly the case a `brew upgrade` cannot reach. NIX_STORE is
# what a Nix builder exports; the literal default covers `nix develop`, where
# it may be absent. Nothing here is Nix-specific by name: any store that hands
# out immutable paths under that root passes.
nix_store = os.environ.get("NIX_STORE") or "/nix/store"


def from_immutable_store(dep):
    paths = [
        flag[flag.index("/"):]
        for flag in [*dep.public_cflags, *dep.ldflags]
        if "/" in flag
    ]
    return bool(paths) and all(path.startswith(nix_store + "/") for path in paths)


def optional_pkg(*pkgs):
    dep = pkg_config(*pkgs, required=False)
    if darwin and not from_immutable_store(dep):
        # A Homebrew or hand-built prefix, or nothing at all: the default macOS
        # build stays the library-free one 62cef373 made it. Disabling the same
        # object pkg_config returned keeps one dependency where there was one
        # before - the runner drops a disabled target from compile flags and
        # link flags alike, so no half of it can survive on its own.
        dep.enabled = False
    return dep


freetype = optional_pkg("freetype2")
fontconfig = optional_pkg("fontconfig")
harfbuzz = optional_pkg("harfbuzz")
brotli_common = optional_pkg("libbrotlicommon")
simdutf = optional_pkg("simdutf >= 6.5.0")
if simdutf:
    # The define rides on the dependency that carries -lsimdutf, and the runner
    # skips a disabled target for compile flags and link flags alike, so a
    # translation unit can only see HAVE_SIMDUTF where the library is also on
    # the link line. base64.cpp used to decide this for itself with
    # __has_include, and under Nix on macOS - header on the include path from
    # buildInputs, the pkg-config lookup above switched off by platform - it
    # compiled calls into a library nobody linked.
    simdutf.public_cppflags += ["-DHAVE_SIMDUTF=1"]

have_freetype_backend = bool(freetype and harfbuzz)
if have_freetype_backend:
    # Each define rides on the dependency that carries its -l flag, the way
    # HAVE_SIMDUTF does above: the runner walks a disabled target out of the
    # compile flags and the link flags together, so "define set, library
    # absent" has nowhere to live. The global build.cppflags these used to sit
    # in were a second place to say the same thing.
    freetype.public_cppflags += ["-DHAVE_FREETYPE=1"]
    harfbuzz.public_cppflags += ["-DHAVE_HARFBUZZ=1"]
    if fontconfig:
        fontconfig.public_cppflags += ["-DHAVE_FONTCONFIG=1"]
else:
    # No backend: switch the very objects pkg_config returned off, rather than
    # replacing them with fresh empty ones. dependency() with no arguments is
    # *enabled*, so the replacements were true in a boolean context - and
    # SHITTY_TEST_FONTCONFIG below, which asks exactly that, reported a
    # fontconfig that no target links and no translation unit can see (G13).
    for font_dependency in (freetype, fontconfig, harfbuzz, brotli_common):
        font_dependency.enabled = False

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

threads = dependency(ldflags=["-pthread"])

vulkan = dependency()
wayland_backend = dependency()
if linux:
    vulkan = pkg_config("vulkan")
    # Nothing injects these into LDFLAGS outside the Nix shell; the
    # Linux backend has to ask for them itself (issue 66).
    wayland_backend = pkg_config("wayland-client", "xkbcommon")
    wayland_backend.ldflags += ["-lrt"]
    build.cppflags += ["-DHAVE_VULKAN_WAYLAND=1"]


embedded_path_flags = [
    "-Wno-error",
    "-ffile-prefix-map=$(S)=.",
    "-ffile-prefix-map=$(B)=.",
]


# libstd picks its backends with __has_include, so which libraries the
# archive needs at link time depends on the headers this target has. An
# imported graph exports outputs, not flags, so nothing carries them over
# the boundary: the importer has to run the same probe and ask for them
# itself, the way the Linux backend does above.
libstd_backends = []
# std/thr/wait_queue.cpp uses the generic 16-byte __atomic builtins on x86-64.
# GCC may lower them to libatomic calls even with -mcx16 (notably on musl).
# Keep the runtime after the imported static archive through its usage flags.
if linux and build.target.startswith("x86_64"):
    libstd_backends.append("-latomic")
# std/thr/io_uring.cpp, pulled in by the reactor every binary starts.
if have_header("liburing.h"):
    libstd_backends.append("-luring")
# std/str/hash.cpp prefers rapidhash, which is header-only, and falls
# back to xxhash before its own FNV-1a. Mirror that order exactly:
# probing for xxhash alone would link a library nothing calls.
if not have_header("rapidhash.h") and have_header("xxhash.h"):
    libstd_backends.append("-lxxhash")
# The TLS and DNS backends are detected the same way but stay out of this
# list: nothing here references them, so the linker never pulls their
# archive members in and their libraries would be dead weight.

libstd = import_build(std_build, "libstd.a", extra_cflags=embedded_path_flags)
libstd.ldflags += libstd_backends
libstd_external_clock = import_build(
    std_build,
    "libstd_external_clock.a",
    extra_cflags=embedded_path_flags,
    extra_cppflags=["-DSTL_EXTERNAL_MONOTONIC_NOW_US=1"],
)
libstd_external_clock.ldflags += libstd_backends


if "-lplt" in build.ldflags:
    plt = dependency(ldflags=["-lplt"])
elif os.path.isfile(os.path.join(os.path.dirname(__file__), plt_build)):
    plt = import_build(
        plt_build,
        "libplt.a",
        extra_cflags=embedded_path_flags,
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
            extra_cflags=embedded_path_flags,
            extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
            deps=[libstd],
        ),
    ]
    if linux:
        plt_test_programs.append(import_build(
            plt_build,
            "plt_wayland_integration_tests",
            extra_cflags=embedded_path_flags,
            extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
            deps=[libstd],
        ))
    plt_tests = untimed_command(
        name="plt_tests",
        inputs=["$(S)/ext/plt/tests/run_timed.py"],
        outputs=["$(B)/plt-tests.stamp"],
        deps=plt_test_programs,
        cmd=[
            # The same hard per-invocation timeout the nested suite uses.
            *[
                ["python3", "$(S)/ext/plt/tests/run_timed.py", "120", program.output]
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
        # render_push_constants.h is an input because the generator reads
        # the fill-pass bit and the transparency field out of it (R9-3,
        # T10). Without it here, moving one of those numbers rebuilds the
        # C++ and leaves the shader compiled from the old one - which is
        # the very drift the single definition exists to prevent, arriving
        # through the build graph instead of through a second declaration.
        inputs=["$(S)/lib/shitty/render.comp", "$(S)/lib/shitty/render_push_constants.h", "$(S)/lib/shitty/generate_render_shaders.py"],
        outputs=[render_shader_output],
        cmd=[
            "python3",
            "$(S)/lib/shitty/generate_render_shaders.py",
            "compile",
            "$(S)/lib/shitty/render.comp",
            render_shader_name,
            render_shader_output,
            "glslangValidator",
        ],
        descr="SH",
        color="magenta",
    ))

render_spv = command(
    name="render_spv",
    inputs=["$(S)/lib/shitty/generate_render_shaders.py", *render_shader_outputs],
    outputs=["$(B)/render_spv.h"],
    deps=render_shader_targets,
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_render_shaders.py",
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
        # render_push_constants.h is an input because the generator reads
        # the fill-pass bit and the transparency field out of it (R9-3,
        # T10). Without it here, moving one of those numbers rebuilds the
        # C++ and leaves the shader compiled from the old one - which is
        # the very drift the single definition exists to prevent, arriving
        # through the build graph instead of through a second declaration.
        inputs=["$(S)/lib/shitty/render.comp", "$(S)/lib/shitty/render_push_constants.h", "$(S)/lib/shitty/generate_render_shaders.py"],
        outputs=["$(B)/render_msl.h"],
        cmd=[
            "python3",
            "$(S)/lib/shitty/generate_render_shaders.py",
            "metal",
            "$(S)/lib/shitty/render.comp",
            "$(B)/render_msl.h",
            "glslangValidator",
            "spirv-cross",
        ],
        descr="SH",
        color="magenta",
    )

# Ragel 6 and 7 are both accepted. Ragel 7 is language-aware and
# dropped -C, renumbered the code styles (its -G1 is a switch-driven
# generator that miscompiles scanners in 7.0.4, so production maps to
# -G0 there), and lost -x, which the parser totality checker needs -
# under ragel 7 that check simply does not run.
def ragel_major() -> int:
    text = subprocess.check_output(["ragel", "--version"], text=True)
    for token in text.split():
        if token[0].isdigit():
            return int(token.split(".")[0])
    raise RuntimeError("cannot parse ragel --version output")

ragel_is_6 = ragel_major() < 7
ragel_prod_flags = ["-C", "-G1", "-L"] if ragel_is_6 else ["-G0", "-L"]
ragel_test_flags = ["-C", "-T1", "-L"] if ragel_is_6 else ["-T1", "-L"]

# Coverage instrumentation with runtime counter relocation chokes on the
# goto-driven automata: the generated VT parser goes from a 54-second
# object to an 800-second one. The coverage tier measures lines of the
# .rl sources, not the codegen style, so it builds the production
# automata table-driven like the test parser already is.
if "-fcoverage-mapping" in os.environ.get("CXXFLAGS", ""):
    ragel_prod_flags = ragel_test_flags

totality_deps = []
if ragel_is_6:
    parser_totality = command(
        name="parser_totality",
        inputs=["$(S)/lib/vterm/parser.rl", "$(S)/lib/vterm/check_parser_totality.py"],
        outputs=["$(B)/parser.rl.total"],
        cmd=[
            "python3",
            "$(S)/lib/vterm/check_parser_totality.py",
            "$(S)/lib/vterm/parser.rl",
            "$(B)/parser.rl.total",
        ],
        descr="RG",
        color="magenta",
    )
    totality_deps = [parser_totality]

parser_prod = command(
    name="parser_prod",
    inputs=["$(S)/lib/vterm/parser.rl"],
    outputs=["$(B)/parser.rl.h"],
    deps=totality_deps,
    cmd=[
        "ragel",
        *ragel_prod_flags,
        "-o",
        "$(B)/parser.rl.h",
        "$(S)/lib/vterm/parser.rl",
    ],
    descr="RG",
    color="magenta",
)

unicode_data_inputs = [
    "$(S)/lib/vterm/unicode_data.py",
    "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
    "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
    "$(S)/ext/unicode/EastAsianWidth-8.0.0.txt",
    "$(S)/ext/unicode/EastAsianWidth-15.0.0.txt",
    "$(S)/ext/unicode/EastAsianWidth-17.0.0.txt",
    "$(S)/ext/unicode/GraphemeBreakProperty-17.0.0.txt",
    "$(S)/ext/unicode/IndicSyllabicCategory-17.0.0.txt",
    "$(S)/ext/unicode/emoji-data-17.0.0.txt",
    "$(S)/ext/unicode/emoji-variation-sequences-17.0.0.txt",
]
unicode_data = command(
    name="unicode_data",
    inputs=unicode_data_inputs,
    outputs=["$(B)/unicode_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/vterm/unicode_data.py",
        "$(S)/ext/unicode",
        "$(B)/unicode_data.h",
    ],
    descr="UD",
    color="magenta",
)


# No totality check here: unlike the VT stream, the config parser is allowed
# to reject input, so unhandled bytes are ordinary syntax errors.
toml_prod = command(
    name="toml_prod",
    inputs=["$(S)/lib/shitty/toml.rl"],
    outputs=["$(B)/toml.rl.h"],
    cmd=[
        "ragel",
        *ragel_prod_flags,
        "-o",
        "$(B)/toml.rl.h",
        "$(S)/lib/shitty/toml.rl",
    ],
    descr="RG",
    color="magenta",
)

parser_test = command(
    name="parser_test",
    inputs=["$(S)/lib/vterm/parser.rl"],
    outputs=["$(B)/parser_test.rl.h"],
    deps=totality_deps,
    cmd=[
        "ragel",
        *ragel_test_flags,
        "-o",
        "$(B)/parser_test.rl.h",
        "$(S)/lib/vterm/parser.rl",
    ],
    descr="RG",
    color="magenta",
)


input_keys = command(
    name="input_keys",
    inputs=["$(S)/lib/shitty/generate_input_keys.py", "$(S)/ext/plt/input.h"],
    outputs=["$(B)/input_keys.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_input_keys.py",
        "$(S)/ext/plt/input.h",
        "$(B)/input_keys.h",
    ],
    descr="DF",
    color="magenta",
)


def icon_png(name, svg, png):
    # The rasterizer comes from the ambient environment: rsvg-convert
    # when present, otherwise svg2png, which has no output flag and
    # drops <input name>.png into its working directory.
    if shutil.which("rsvg-convert") is not None or shutil.which("svg2png") is None:
        cmd = ["rsvg-convert", "-w", "1024", "-h", "1024", svg, "-o", png]
    else:
        produced = "$(B)/" + Path(svg).name + ".png"
        cmd = [
            ["svg2png", svg, "1024x1024"],
            [
                "python3",
                "-c",
                f"import os; os.replace(r'{produced}', r'{png}')",
            ],
        ]
    return command(
        name=name,
        inputs=[svg],
        outputs=[png],
        cmd=cmd,
        descr="SV",
        color="magenta",
    )


shitty_icon_png = icon_png("shitty_icon_png", "$(S)/bin/st/shitty.svg", "$(B)/shitty.png")


shitty_icon_data = command(
    name="shitty_icon_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/shitty.png",
    ],
    deps=[shitty_icon_png],
    outputs=["$(B)/shitty_icon_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/shitty_icon_data.h",
        "shittyIcon=$(B)/shitty.png",
    ],
    descr="IC",
    color="magenta",
)


pretty_icon_png = icon_png("pretty_icon_png", "$(S)/bin/pt/pretty.svg", "$(B)/pretty.png")


pretty_icon_data = command(
    name="pretty_icon_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/pretty.png",
    ],
    deps=[pretty_icon_png],
    outputs=["$(B)/pretty_icon_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/pretty_icon_data.h",
        "prettyIcon=$(B)/pretty.png",
    ],
    descr="IC",
    color="magenta",
)


# The brand's example config, embedded so -printConfig can write it back
# out. Same generator as the icons above - it takes any file and emits a
# byte array - and per brand for the same reason: bin/pt/pretty.toml is
# the one pt has to print, and shipping shitty.toml inside pt would put
# the forbidden substring in it (tst/pretty_binary_branding.py).
shitty_config_data = command(
    name="shitty_config_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(S)/bin/st/shitty.toml",
    ],
    outputs=["$(B)/shitty_config_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/shitty_config_data.h",
        "shittyExampleConfig=$(S)/bin/st/shitty.toml",
    ],
    descr="EC",
    color="magenta",
)


pretty_config_data = command(
    name="pretty_config_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(S)/bin/pt/pretty.toml",
    ],
    outputs=["$(B)/pretty_config_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/pretty_config_data.h",
        "prettyExampleConfig=$(S)/bin/pt/pretty.toml",
    ],
    descr="EC",
    color="magenta",
)


font_coverage = command(
    name="font_coverage",
    inputs=[
        "$(S)/lib/shitty/generate_font_coverage.py",
        "$(S)/ext/fonts/NotoColorEmoji.ttf",
        "$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    outputs=["$(B)/font_coverage.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_coverage.py",
        "$(B)/font_coverage.h",
        "$(S)/ext/fonts/NotoColorEmoji.ttf",
        "$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    descr="DF",
    color="magenta",
)


font_data = command(
    name="font_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/ext/fonts/NotoColorEmoji.ttf",
        "$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    outputs=["$(B)/font_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/font_data.h",
        "embeddedFontMono=$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "embeddedFontEmoji=$(S)/ext/fonts/NotoColorEmoji.ttf",
        "embeddedFontEmojiText=$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    descr="FD",
    color="magenta",
)


# The shell integration's scripts (lib/shitty/shell_integration.cpp), written
# out at startup for the shell to read. One copy for both brands: the scripts
# name neither, which tst/pretty_binary_branding.py holds pt to.
shell_integration_data = command(
    name="shell_integration_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(S)/lib/shitty/shell/zsh/.zshenv",
        "$(S)/lib/shitty/shell/zsh/integration.zsh",
    ],
    outputs=["$(B)/shell_integration_data.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/shell_integration_data.h",
        "zshIntegrationEnv=$(S)/lib/shitty/shell/zsh/.zshenv",
        "zshIntegration=$(S)/lib/shitty/shell/zsh/integration.zsh",
    ],
    descr="SI",
    color="magenta",
)


terminal_colors_data = command(
    inputs=[
        "$(S)/lib/shitty/terminal_colors.json",
        "$(S)/lib/shitty/terminal_colors.py",
    ],
    outputs=["$(B)/terminal_colors.json.h"],
    cmd=[
        "python3",
        "$(S)/lib/shitty/terminal_colors.py",
        "generate",
        "$(B)/terminal_colors.json.h",
    ],
    descr="TC",
    color="magenta",
)


main_source = "$(S)/lib/shitty/main.cpp"
shitty_main_source = "$(S)/bin/st/main.cpp"
pretty_main_source = "$(S)/bin/pt/main.cpp"
fuzz_source = "$(S)/bin/main_fuzz/main.cpp"
heap_profile_source = "$(S)/lib/shitty/heap_profile.cpp"
parser_source = "$(S)/lib/vterm/parser.cpp"
toml_source = "$(S)/lib/shitty/toml.cpp"
toml_dump_source = "$(S)/bin/toml_dump/main.cpp"
parser_perf_source = "$(S)/bin/parser_perf/main.cpp"
unit_sources = sorted(build.glob("$(S)/lib/shitty/*_ut.cpp") + build.glob("$(S)/lib/vterm/*_ut.cpp"))
platform_font_sources = {
    "$(S)/lib/shitty/font_freetype.cpp",
}
platform_renderer_sources = {
    "$(S)/lib/shitty/render_vk.cpp",
    "$(S)/lib/shitty/ui_wayland_chrome.cpp",
}
enabled_font_sources = set()
if have_freetype_backend:
    enabled_font_sources.add("$(S)/lib/shitty/font_freetype.cpp")
enabled_renderer_sources = set()
if linux:
    enabled_renderer_sources.add("$(S)/lib/shitty/render_vk.cpp")
    enabled_renderer_sources.add("$(S)/lib/shitty/ui_wayland_chrome.cpp")
all_libshitty_sources = [
    source for source in build.glob("$(S)/lib/shitty/*.cpp") + build.glob("$(S)/lib/vterm/*.cpp")
    if source not in (heap_profile_source, *unit_sources)
    and (source not in platform_font_sources or source in enabled_font_sources)
    and (source not in platform_renderer_sources or source in enabled_renderer_sources)
]
if darwin:
    all_libshitty_sources.append({
        "src": "$(S)/lib/shitty/render_metal.mm",
        "inputs": ["$(B)/render_msl.h"],
    })
    all_libshitty_sources.append("$(S)/lib/shitty/ui_csd_tabs.mm")
    all_libshitty_sources.append("$(S)/lib/shitty/ui_quick_hotkey.mm")
    all_libshitty_sources.append("$(S)/lib/shitty/ui_sidebar_tabs.mm")
    all_libshitty_sources.append("$(S)/lib/shitty/ui_palette.mm")
vterm_source = "$(S)/lib/vterm/vterm.cpp"
font_embedded_source = "$(S)/lib/shitty/font_embedded.cpp"
application_source = "$(S)/lib/shitty/application.cpp"
terminal_colors_source = "$(S)/lib/shitty/terminal_colors.cpp"
shell_integration_source = "$(S)/lib/shitty/shell_integration.cpp"
grapheme_source = "$(S)/lib/shitty/grapheme.cpp"
unicode_source = "$(S)/lib/vterm/unicode.cpp"
libshitty_sources = [
    {
        "src": source,
        "inputs": ["$(B)/parser.rl.h"],
    } if source == parser_source else {
        "src": source,
        "inputs": ["$(B)/toml.rl.h"],
    } if source == toml_source else {
        "src": source,
        "inputs": ["$(B)/font_data.h", "$(B)/font_coverage.h"],
    } if source == font_embedded_source else {
        "src": source,
        "inputs": ["$(B)/terminal_colors.json.h"],
    } if source == terminal_colors_source else {
        "src": source,
        "inputs": ["$(B)/shell_integration_data.h"],
    } if source == shell_integration_source else {
        "src": source,
        "inputs": ["$(B)/unicode_data.h"],
    } if source == unicode_source else source
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
        "inputs": ["$(B)/font_data.h", "$(B)/font_coverage.h"],
    } if source == font_embedded_source else {
        "src": source,
        "inputs": ["$(B)/terminal_colors.json.h"],
    } if source == terminal_colors_source else {
        "src": source,
        "inputs": ["$(B)/shell_integration_data.h"],
    } if source == shell_integration_source else {
        "src": source,
        "inputs": ["$(B)/unicode_data.h"],
    } if source == unicode_source else source
    for source in all_libshitty_sources
]
libshitty_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, wayland_backend, threads, libstd,
    brotli_common, simdutf,
]
libshitty_test_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, wayland_backend, threads, libstd,
    brotli_common, simdutf,
]
libshitty_fuzz_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, wayland_backend, threads, libstd_external_clock,
    brotli_common, simdutf,
]


libshitty = library(
    srcs=libshitty_sources,
    cxxflags=production_path_flags,
    deps=libshitty_deps,
    output="$(B)/libshitty_prod.a",
)


st = program(
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h", "$(B)/shitty_config_data.h"],
    }],
    cxxflags=production_path_flags,
    deps=[libshitty],
)


pt = program(
    name="pt",
    output="$(B)/pt",
    srcs=[{
        "src": pretty_main_source,
        "inputs": ["$(B)/pretty_icon_data.h", "$(B)/pretty_config_data.h"],
    }],
    cxxflags=production_path_flags,
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
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h", "$(B)/shitty_config_data.h"],
    }, heap_profile_source],
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
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h", "$(B)/shitty_config_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test],
)


pt_test = program(
    name="pt_test",
    output="$(B)/pt_test",
    srcs=[{
        "src": pretty_main_source,
        "inputs": ["$(B)/pretty_icon_data.h", "$(B)/pretty_config_data.h"],
    }],
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
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h", "$(B)/shitty_config_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test_prod_parser],
)


pt_test_prod_parser = program(
    name="pt_test_prod_parser",
    output="$(B)/pt_test_prod_parser",
    srcs=[{
        "src": pretty_main_source,
        "inputs": ["$(B)/pretty_icon_data.h", "$(B)/pretty_config_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test_prod_parser],
)


pty_test_helper = program(
    name="pty_test_helper",
    output="$(B)/pty_test_helper",
    srcs=["$(S)/tst/pty_test_helper.c"],
)


# The kernel-truth probe behind the Darwin-gated pty unit tests; built
# on demand, run by hand on the host being characterized.
pty_probe = program(
    name="pty_probe",
    output="$(B)/pty_probe",
    srcs=["$(S)/tst/pty_probe.c"],
)


unit_tests = program(
    name="unit_tests",
    output="$(B)/unit_tests",
    srcs=["$(S)/ext/libstd/tst/test.cpp", *unit_sources],
    deps=[libshitty_test, libstd],
)


toml_dump = program(
    name="toml_dump",
    output="$(B)/toml_dump",
    srcs=[toml_dump_source],
    deps=[libshitty_test, libstd],
)


# The production parser alone, driven with no-op callbacks; a perf
# probe that isolates the FSM from vterm and the screen.
parser_perf = program(
    name="parser_perf",
    output="$(B)/parser_perf",
    srcs=[parser_perf_source],
    deps=[libshitty, libstd],
)


# The whole VT core - parser, vterm and screen - driven headlessly over
# a corpus file; the throughput an embedder would actually see.
core_perf = program(
    name="core_perf",
    output="$(B)/core_perf",
    srcs=["$(S)/bin/core_perf/main.cpp"],
    deps=[libshitty, libstd],
)


# The C embedding facade links against a lib/vterm of its own now. It could
# not before: vterm.cpp called Composer::contentInsets() and Composer::resize()
# - the two symbols A1/A10 leave on the embedder's side - so an archive globbed
# out of lib/vterm alone had two undefined symbols before the facade was even
# compiled, and lib/embed/shitty_vt.cpp did not compile against our
# eleven-parameter Vterm::create either.
#
# Both are closed: the window's insets, the in-band resize and the pane list's
# cell count reach the core through VtHost (contentInsets, surfaceResized,
# cellCapacityExcept), Vterm::create takes ten parameters and no Composer&, and
# lib/vterm/vt_headless.cpp - the one other file in the core that reached into
# lib/shitty - moved to lib/shitty, where the adapter it is has always
# belonged.
# That last move is what `./build so` needs specifically: link_shared.py takes
# the core archive under --whole-archive with --no-undefined, so a single
# object referring to lib/shitty fails the link whether or not anything calls
# it.
#
# The flag stays as a switch rather than being dissolved, because it names
# exactly what the facade costs: the targets below, `example` in the deps and
# SHITTY_EMBED_EXAMPLE_BINARY in the env of every python test group (both
# written out in make_python_test_groups below and conditional on `example is
# not None`), and the class-level skip in tst/test_embed_example.py, which keys
# on the artifact existing and so lifts itself the first time the binary is
# built.
embed_facade_links = True

if embed_facade_links:
    # The C embedding facade over the VT core: shitty_vt_* in lib/embed,
    # linked with libstd and a headless-only libplt. `./build a` bundles
    # the three into one static archive, `./build so` links the shared
    # library with everything but the facade hidden by the version script.
    # Everything embed-side is compiled -fPIC: `build so` needs it, and a
    # position-independent static archive is what a consumer linking the
    # facade into their own shared object wants anyway.
    plt_headless = import_build(
        plt_build,
        "libplt_headless.a",
        extra_cflags=[*embedded_path_flags, "-fPIC"],
        extra_cxxflags=["-fPIC"],
        extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd", "-Dplatforms=headless"],
    )
    libstd_pic = import_build(
        std_build,
        "libstd_pic.a",
        extra_cflags=[*embedded_path_flags, "-fPIC"],
        extra_cxxflags=["-fPIC"],
    )
    libstd_pic.ldflags += libstd_backends

    embed_sources = [
        {
            "src": source,
            "inputs": ["$(B)/parser.rl.h"],
        } if source == parser_source else {
            "src": source,
            "inputs": ["$(B)/unicode_data.h"],
        } if source == unicode_source else source
        for source in sorted(build.glob("$(S)/lib/vterm/*.cpp"))
        if source not in unit_sources
    ] + ["$(S)/lib/embed/shitty_vt.cpp"]

    libshitty_vt_core = library(
        name="libshitty_vt_core",
        srcs=embed_sources,
        cflags=["-fPIC"],
        cxxflags=[*production_path_flags, "-fPIC"],
        deps=[plt_headless, libstd_pic, simdutf],
        output="$(B)/libshitty_vt_core.a",
    )

    example = program(
        name="example",
        output="$(B)/example",
        srcs=["$(S)/bin/example/main.c"],
        deps=[libshitty_vt_core, plt_headless, libstd_pic, simdutf],
    )

    if linux:
        shitty_vt_a = command(
            name="shitty_vt_a",
            inputs=[
                "$(S)/lib/embed/merge_archives.py",
                libshitty_vt_core.output,
                plt_headless.output,
                libstd_pic.output,
            ],
            outputs=["$(B)/libshitty_vt.a"],
            deps=[libshitty_vt_core, libstd_pic, plt_headless],
            cmd=[[
                "python3",
                "$(S)/lib/embed/merge_archives.py",
                "$(B)/libshitty_vt.a",
                libshitty_vt_core.output,
                plt_headless.output,
                libstd_pic.output,
            ]],
            descr="AR",
            color="magenta",
        )
        group("a", shitty_vt_a)

        shitty_vt_so = command(
            name="shitty_vt_so",
            inputs=[
                "$(S)/lib/embed/link_shared.py",
                "$(S)/lib/embed/shitty_vt.map",
                libshitty_vt_core.output,
                plt_headless.output,
                libstd_pic.output,
            ],
            outputs=["$(B)/libshitty_vt.so"],
            deps=[libshitty_vt_core, libstd_pic, plt_headless],
            cmd=[[
                "python3",
                "$(S)/lib/embed/link_shared.py",
                "$(B)/libshitty_vt.so",
                "$(S)/lib/embed/shitty_vt.map",
                libshitty_vt_core.output,
                plt_headless.output,
                libstd_pic.output,
                *simdutf.ldflags,
                # libstd's hash, atomic and io_uring backends are probed, so the
                # shared link needs whatever the probe chose; --no-undefined
                # rejects the library outright without them.
                *libstd_backends,
            ]],
            descr="SO",
            color="magenta",
        )
        group("so", shitty_vt_so)

        # The release tarball: both libraries, the header, and a pkg-config
        # file carrying the link flags this build probed - the discovery
        # story of issue 102. Relocatable; point PKG_CONFIG_PATH at its
        # lib/pkgconfig after unpacking.
        shitty_vt_tgz = command(
            name="shitty_vt_tgz",
            inputs=[
                "$(S)/lib/embed/make_release.py",
                "$(S)/lib/embed/shitty_vt.h",
                "$(B)/libshitty_vt.a",
                "$(B)/libshitty_vt.so",
            ],
            outputs=["$(B)/shitty_vt.tgz"],
            deps=[shitty_vt_a, shitty_vt_so],
            cmd=[[
                "python3",
                "$(S)/lib/embed/make_release.py",
                "$(B)/shitty_vt.tgz",
                shitty_version,
                "$(S)/lib/embed/shitty_vt.h",
                "$(B)/libshitty_vt.a",
                "$(B)/libshitty_vt.so",
                "--",
                *simdutf.ldflags,
                *libstd_backends,
                "-lpthread",
                "-lm",
            ]],
            descr="TZ",
            color="magenta",
        )
        group("tgz", shitty_vt_tgz)
else:
    example = None


# Each shard is an independent graph node with its own hard timeout.
test_group_count = 20
python_test_inputs = [
    "$(S)/build",
    "$(S)/build.py",
    "$(S)/README.md",
    *build.glob("$(S)/LICENSE.*"),
    "$(S)/dev/ci_report.py",
    "$(S)/lib/shitty/heap_profile.cpp",
    "$(S)/bin/main_fuzz/main.cpp",
    "$(S)/bin/parser_perf/main.cpp",
    "$(S)/bin/core_perf/main.cpp",
    *build.glob("$(S)/lib/shitty/*_ut.cpp"),
    *build.glob("$(S)/lib/vterm/*_ut.cpp"),
    *build.glob("$(S)/tst/*.py"),
    *build.glob("$(S)/tst/*.md"),
    # test_zsh_integration.py runs these under a real zsh.
    "$(S)/lib/shitty/shell/zsh/.zshenv",
    "$(S)/lib/shitty/shell/zsh/integration.zsh",
    "$(S)/tst/pty_test_helper.c",
    *build.glob("$(S)/ext/fonts/*"),
    # The color-scheme suite reads the imported theme licenses, the
    # embed differential replays the recorded fuzz corpus.
    *build.glob("$(S)/ext/LICENSE.*"),
    *build.glob("$(S)/tst/corpus/*"),
    *build.glob("$(S)/tst/**/*file_names.txt"),
    *build.glob("$(S)/tst/**/xfail.txt"),
    *build.glob("$(S)/tst/contour/vttest/*"),
    "$(S)/tst/termless/cases.json",
    "$(S)/tst/termless/upstream/LICENSE",
    *build.glob("$(S)/tst/termless/upstream/**/*.ts"),
    "$(S)/tst/tmux/upstream/input-fuzzer.dict",
    *[
        "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
        for path in sorted((Path(__file__).parent / "tst" / "toml").rglob("*"))
        if path.is_file()
    ],
    *build.glob("$(S)/tst/ucs_detect/*.txt"),
    *build.glob("$(S)/tst/vte/upstream/parser-*.hh"),
    *build.glob("$(S)/tst/vtebench/benchmarks/*/*"),
    "$(S)/tst/wezterm/catalog.py",
    "$(S)/tst/windows_terminal/upstream/KittyKeyboardProtocol.cpp",
    *build.glob("$(S)/tst/windows_terminal/upstream/*Test*.cpp"),
    "$(S)/tst/wraptest/cases.json",
    "$(S)/tst/wraptest/wraptest.c",
    *build.glob("$(S)/tst/xterm_vttests/upstream/*"),
    "$(S)/ext/libstd/build.py",
    *build.glob("$(S)/ext/libstd/**/*_ut.cpp"),
    *build.glob("$(S)/ext/libstd/tst/*.cpp"),
    "$(S)/ext/plt/build.py",
    *build.glob("$(S)/ext/plt/*_ut.cpp"),
    *build.glob("$(S)/ext/plt/tests/*"),
    "$(S)/lib/shitty/application.cpp",
    "$(S)/bin/pt/pretty.desktop",
    "$(S)/bin/pt/pretty.toml",
    "$(S)/bin/st/shitty.desktop",
    "$(S)/bin/st/shitty.toml",
    "$(S)/lib/shitty/terminal_colors.json",
    "$(S)/lib/shitty/terminal_colors.py",
    *unicode_data_inputs,
    "$(S)/ext/unicode/GraphemeBreakTest-17.0.0.txt",
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
        deps=[unit_tests, pty_test_helper],
        cmd=[
            [
                "$(B)/unit_tests",
                f"--group={group_index}",
                f"--group-count={test_group_count}",
                "--threads=1",
            ],
            touch_stamp(output),
        ],
        env={"SHITTY_PTY_TEST_HELPER": "$(B)/pty_test_helper"},
        # A group holds real-child pty tests with multi-second sleeps, and
        # continuous-mode coverage instrumentation slows the rest; on a
        # 4-vCPU CI runner the default 60-second budget is too tight.
        test_timeout_seconds=180,
        descr="UT",
        color="green",
    ))


def make_python_test_groups(name, output_directory, test_binary, test_target, pretty_test_binary, pretty_test_target, descr):
    result = []

    for group_index in range(test_group_count):
        output = f"$(B)/{output_directory}/group-{group_index:02}.stamp"
        result.append(command(
            name=f"{name}_group_{group_index:02}",
            inputs=python_test_inputs,
            outputs=[output],
            # example joins this list with embed_facade_links above; until
            # then tst/test_embed_example.py runs without a binary and
            # reports the gap instead of hiding it.
            deps=[test_target, pretty_test_target, toml_dump, *([example] if example is not None else [])],
            cmd=[
                [
                    "python3",
                    "tst/run_unittest_group.py",
                    f"--group={group_index}",
                    f"--group-count={test_group_count}",
                ],
                touch_stamp(output),
            ],
            cwd="$(S)",
            env={
                "SHITTY_TEST_BINARY": test_binary,
                **({"SHITTY_EMBED_EXAMPLE_BINARY": "$(B)/example"} if example is not None else {}),
                "SHITTY_PRETTY_TEST_BINARY": pretty_test_binary,
                "SHITTY_TOML_DUMP_BINARY": "$(B)/toml_dump",
                "SHITTY_TEST_FONTCONFIG": "1" if fontconfig else "0",
                "SHITTY_TEST_PLATFORM": "cocoa" if darwin else "wayland",
                "SHITTY_TEST_VERSION": shitty_version,
            },
            # A group contains about 210 independent tests.  Keep the
            # per-test default at 60 seconds, but allow the complete group to
            # absorb slow sandbox-only metadata probes without racing its own
            # aggregate timeout.
            test_timeout_seconds=120,
            descr=descr,
            color="cyan",
        ))

    return result


python_test_groups = make_python_test_groups(
    "test_suite",
    "python-tests",
    "$(B)/st_test",
    st_test,
    "$(B)/pt_test",
    pt_test,
    "TS",
)
python_test_prod_parser_groups = make_python_test_groups(
    "test_suite_prod_parser",
    "python-tests-prod-parser",
    "$(B)/st_test_prod_parser",
    st_test_prod_parser,
    "$(B)/pt_test_prod_parser",
    pt_test_prod_parser,
    "TP",
)


pretty_binary_branding = command(
    name="pretty_binary_branding",
    inputs=["$(S)/tst/pretty_binary_branding.py"],
    outputs=["$(B)/tst/pretty-binary-branding.stamp"],
    deps=[pt],
    cmd=[
        ["python3", "tst/pretty_binary_branding.py", "$(B)/pt"],
        touch_stamp("$(B)/tst/pretty-binary-branding.stamp"),
    ],
    cwd="$(S)",
    descr="PB",
    color="cyan",
)


production_surface = command(
    inputs=[
        "$(S)/tst/production_surface.py",
        "$(S)/bin/pt/pretty.toml",
    ],
    outputs=["$(B)/tst/production-surface.stamp"],
    deps=[st, pt],
    cmd=[
        ["python3", "tst/production_surface.py"],
        touch_stamp("$(B)/tst/production-surface.stamp"),
    ],
    cwd="$(S)",
    env={
        "SHITTY_PRODUCTION_BINARY": "$(B)/st",
        "SHITTY_PRETTY_BINARY": "$(B)/pt",
        "SHITTY_TEST_VERSION": shitty_version,
    },
    descr="PS",
    color="cyan",
)


# Issue #104's invariant on the production binary: PTY progress must
# survive the compositor withholding frame callbacks. Runs a headless
# sway and powers its output off; skips without sway or a Vulkan
# device unless SHITTY_TEST_WAYLAND_REQUIRED=1 (CI sets it).
wayland_frame_stall = command(
    name="wayland_frame_stall",
    inputs=["$(S)/tst/wayland_frame_stall.py"],
    outputs=["$(B)/tst/wayland-frame-stall.stamp"],
    deps=[st],
    cmd=[
        ["python3", "tst/wayland_frame_stall.py"],
        touch_stamp("$(B)/tst/wayland-frame-stall.stamp"),
    ],
    cwd="$(S)",
    env={"SHITTY_PRODUCTION_BINARY": "$(B)/st"},
    test_timeout_seconds=120,
    descr="WF",
    color="cyan",
)


# Issue #108's invariant on the production binary: the foreground
# process name fills the title while the app sets none, and a standing
# foreground keeps the title it set. Same sway harness and skip rules
# as wayland_frame_stall.
wayland_title_fallback = command(
    name="wayland_title_fallback",
    inputs=[
        "$(S)/tst/wayland_title_fallback.py",
        "$(S)/tst/wayland_frame_stall.py",
    ],
    outputs=["$(B)/tst/wayland-title-fallback.stamp"],
    deps=[st],
    cmd=[
        ["python3", "tst/wayland_title_fallback.py"],
        touch_stamp("$(B)/tst/wayland-title-fallback.stamp"),
    ],
    cwd="$(S)",
    env={"SHITTY_PRODUCTION_BINARY": "$(B)/st"},
    test_timeout_seconds=120,
    descr="WT",
    color="cyan",
)


# A1 says the scalar border is the user's option and Composer::contentInsets()
# is the only source of layout geometry. Nothing in the tree enforced that: the
# migration off the scalar was checked by grep once, and a single call put back
# anywhere would compile, link, pass every test, and silently lay out a window
# as if no chrome reserved anything (R3-test, I3). This is that grep, wired to
# a build step so it runs again on every change instead of once in a review.
#
# Both names, not one. The node was written against borderPixels() alone, and
# the same commit published Composer::scaledPixels() - which *is* the body of
# borderPixels(), one multiplication away from the option. A layout that reads
# `composer.scaledPixels(composer.opts->border)` is the exact rollback this
# node exists to stop, and it passed the node green (R4-test, I1) while looking
# sanctioned: the name is public, documented and new.
#
# The allowance is per file, and a count where a count is what makes it tight:
# composer.{h,cpp} own both functions and are the only place layout may spell
# them, so their allowance is the number of times they do it today - a count
# there is what keeps Composer::resize() from quietly growing a scalar of its
# own (R4-qa, Q4). test_mode.cpp reports the option's value in its FONT_STATE
# answer, which is a report and not a layout (R3-arch examined it and let it
# stand), and one is how many of those there is. The two _ut.cpp files read the
# scalar back to check it against the insets, which is the one legitimate
# reason to name it outside Composer, and there is no useful number to write
# down for a test file that grows.
#
# T2.1: what is counted is a *call*, through blanked source, and that is three
# separate repairs to the same scan (T0.3, section 3.2 and 3.4).
#
# The scan used to be str.count on raw text, so prose counted. Two thirds of
# composer.h's old allowance of five was the file explaining itself, and
# grid_geometry.h's whole allowance of one was a comment - G1 walked into this
# from the other side and found that a file could spend its allowance on prose
# and let a real call through for free. Reading blanked source, the way the
# other three guards already do, ends both halves of that: the numbers below
# are calls and only calls, which is why they went down and why
# grid_geometry.h left the map rather than being re-keyed by T5.2 later.
#
# Requiring the bracket is what keeps the guard usable across the merge at all.
# The upstream core carries a VtGeometry::borderPixels field, so every
# `geometry.borderPixels` read in upstream code contains the name: measured on
# a clean origin/master tree, about 37 substring hits in 8 files, not one of
# them an A1 violation. A guard that red-lines on legitimate upstream code is a
# guard someone widens the allowance to silence, which is how A1 dies quietly
# inside a green node. Ours are methods and are always spelled with the
# bracket; the field is always read without one, so the bracket separates them
# and no allowance had to grow.
border_pixels_names = ("borderPixels", "scaledPixels")
border_pixels_allowance = {
    "lib/shitty/composer.h": 2,
    "lib/shitty/composer.cpp": 8,
    "lib/shitty/composer_ut.cpp": None,
    "lib/shitty/mouse_frontend_ut.cpp": None,
    "lib/shitty/test_mode.cpp": 1,
}

# The roots the scan walks and the files the node depends on have to be the
# same set, or a call could be added where nothing re-runs the check. They are
# now one expression rather than two lists that agreed by hand and had already
# stopped agreeing on the subdirectories (R4-test, Z3). libstd is left out of
# both on purpose: it is a vendored standard library and has never heard of
# Composer.
#
# All three source-scanning guards below share this one set: a file that can
# hide a border call can hide a single-pane pointer geometry or an unguarded
# darwin call just as easily, and three lists that had to agree by hand is how
# Z3 happened once already.
#
# lib/vterm joined the set in T2.1, with M3. The VT core is moving out of
# lib/shitty a commit at a time, and a root the scan does not walk is a root
# where every one of these four checks passes by having nothing to read.
#
# lib/embed joined it in T6.1, for the same reason one merge later: the C
# facade is a new product directory, and until it was named here nothing
# guarded it at all. Probed rather than reasoned about - the line
# `mouseGeometry(composer); createCsdTabsUi(composer); composer.borderPixels()`
# written as code into lib/embed/shitty_vt.cpp left all four guards green (M7,
# probe 6; re-measured in T6.1 before this line changed).
#
# .c joined the suffixes in the same task, and it is the other half of the same
# hole seen from the other side: bin has been a root since the beginning, but
# bin/example/main.c is the tree's only .c file and the scan walked straight
# past it. A root the scan does not walk and a file the scan does not read are
# the same blindness, and the second one is worse for being invisible - the
# directory is right there in the list.
guard_scan_roots = ("lib/shitty", "lib/vterm", "lib/embed", "ext/plt", "bin")
guard_scan_suffixes = (".cpp", ".h", ".mm", ".c")
guard_scan_sources = sorted(set(
    source
    for root in guard_scan_roots
    for suffix in guard_scan_suffixes
    for source in build.glob(f"$(S)/{root}/**/*{suffix}")
))

# Every guard below reads source rather than symbols, so each has to see it the
# way the compiler does: comments and string bodies are replaced by spaces, one
# character for one character, and never deleted. R5-qa's own one-off audit
# collapsed block comments instead, moved every line after them, and reported
# three calls on the wrong side of an #if - an instrument that "finds problems"
# is more convincing than a silent one, which is exactly why it needs controls
# on both sides.
#
# border_pixels_guard joined the other three here in T2.1; it had been counting
# raw substrings, prose included, which is the whole of G1's second finding.
guard_source_reader = r"""
import pathlib
import re
import sys


def blanked(text):
    out = list(text)
    index = 0
    size = len(text)
    while index < size:
        char = text[index]
        if char == "/" and index + 1 < size and text[index + 1] == "/":
            while index < size and text[index] != "\n":
                out[index] = " "
                index += 1
        elif char == "/" and index + 1 < size and text[index + 1] == "*":
            out[index] = out[index + 1] = " "
            index += 2
            while index < size and not (text[index] == "*" and index + 1 < size and text[index + 1] == "/"):
                if text[index] != "\n":
                    out[index] = " "
                index += 1
            if index < size:
                out[index] = out[index + 1] = " "
                index += 2
        elif char in "\"'":
            quote = char
            index += 1
            while index < size and text[index] != quote:
                if text[index] == "\\" and index + 1 < size:
                    out[index] = " "
                    index += 1
                if index < size:
                    if text[index] != "\n":
                        out[index] = " "
                    index += 1
            index += 1
        else:
            index += 1
    return "".join(out)


def scanned(roots, suffixes):
    for root in roots:
        for path in sorted(pathlib.Path(root).rglob("*")):
            if path.suffix in suffixes and path.is_file():
                yield path, blanked(path.read_text())


def closing(text, at):
    depth = 0
    while at < len(text):
        if text[at] == "(":
            depth += 1
        elif text[at] == ")":
            depth -= 1
            if depth == 0:
                return at
        at += 1
    return None
"""


border_pixels_guard_program = guard_source_reader + r"""
allowance = %r
names = %r
bad = []
seen = set()
for path, text in scanned(%r, %r):
    key = path.as_posix()
    seen.add(key)
    # Whole-text and not line-by-line: `borderPixels\n()` is absurd to write
    # and trivial to hide behind, and a guard has to survive being written
    # around on purpose.
    hits = [
        f"{key}:{line}"
        for line in sorted(
            text.count(chr(10), 0, match.start()) + 1
            for name in names
            for match in re.finditer(r"\b" + name + r"\s*\(", text)
        )
    ]
    if not hits:
        continue
    if key not in allowance:
        bad += hits
    elif allowance[key] is not None and len(hits) > allowance[key]:
        bad += hits[allowance[key]:]
if bad:
    sys.stderr.write(
        "borderPixels()/scaledPixels() are the border option and its scale, "
        "not the layout (A1): contentInsets() is what layout reads.\n"
        "Unallowed uses:\n  " + "\n  ".join(bad) + "\n"
    )
    sys.exit(1)
stale = sorted(set(allowance) - seen)
if stale:
    sys.stderr.write(
        "the border audit is allowing files the scan never reached, so it is "
        "guarding a tree that no longer exists: re-key the allowance onto "
        "where these live now, or drop them.\n"
        "Unreachable:\n  " + "\n  ".join(stale) + "\n"
    )
    sys.exit(1)
""" % (border_pixels_allowance, border_pixels_names, guard_scan_roots, guard_scan_suffixes)

border_pixels_guard = untimed_command(
    name="border_pixels_guard",
    inputs=["$(S)/build.py", *guard_scan_sources],
    outputs=["$(B)/tst/border-pixels-guard.stamp"],
    cmd=[
        ["python3", "-c", border_pixels_guard_program],
        touch_stamp("$(B)/tst/border-pixels-guard.stamp"),
    ],
    cwd="$(S)",
    descr="BP",
    color="cyan",
)


# T5-4 (R5-test). mouseGeometry(const Composer&) means "the pane fills the
# window", and it has no production caller: the four real ones all hand over an
# origin. Nothing said so, though, and the day "pane == window" stops being true
# a new single-argument call from production compiles, links, passes every test
# and silently maps a pointer as if every pane began at the window's own origin -
# the loss A8 spends a separate pair of fields to prevent.
#
# Tests keep the form unmetered - _ut.cpp is skipped outright - because a
# MouseGeometry for a whole-window pane is a thing a test may still want to
# spell. Everything else is counted.
#
# T6.1 re-measured that skip rather than inheriting it, on the widened scan:
# lifting it costs nothing today, because there is not one single-argument call
# anywhere in the tree, _ut.cpp included - the thirteen call sites are all
# two-argument. The skip is kept anyway, and the reason is what it meters and
# not what it currently finds: A8 is about what production may assume, a test
# is not production, and a guard that red-lines a legitimate test is a guard
# whose allowance grows a test-file key - which is the failure this file spends
# three comments warning about. The number that would change if that reasoning
# ever stops holding is zero, so lifting it stays cheap.
#
# Comments cannot trip this: the source is blanked before it is read, so a
# comment naming mouseGeometry() is spaces by the time the scan gets there.
#
# M6b is the day it moved: the keys followed mouse_frontend into lib/vterm and
# the counts stayed 1 and 1, metering the declaration and the definition, which
# were single-argument then. T2.1 built the stale-key red for precisely that
# moment, and the alternative it was built against - a key nothing reaches,
# green over a violation it can no longer see - is what T0.3 proved by probe.
#
# T5.1 is the day the subject of those two counts stopped existing. The
# single-argument overload was not moved but deleted: mouse_frontend.h declares
# one mouseGeometry and it takes the pane's geometry and the window's, and
# mouse_frontend.cpp defines that one. Measured on this tree, the scan finds
# zero single-argument uses anywhere outside _ut.cpp - so both counts had
# become a pardon for an offence nobody was committing, and an allowance no
# subject supports is the same shape of blindness the stale-key red was built
# against, one step further along.
#
# T5.8 narrows them to zero, which is where the form's absence puts them. It is
# not cosmetic: an allowance of 1 pardons the *first* hit in the file it names,
# and the first hit in mouse_frontend.h is exactly where the overload would
# come back. Re-declaring `MouseGeometry mouseGeometry(const VtGeometry&);`
# there costs one hit, stays under a count of 1, and leaves the guard green
# over the return of the very form it exists to forbid - probed both ways in
# the T5.8 report. The keys stay rather than being dropped, so the stale-key
# check keeps asserting that the guard can still see the two files that form
# would come back to.
mouse_geometry_allowance = {
    "lib/vterm/mouse_frontend.h": 0,
    "lib/vterm/mouse_frontend.cpp": 0,
}

mouse_geometry_guard_program = guard_source_reader + r"""
allowance = %r
bad = []
seen = set()
for path, text in scanned(%r, %r):
    key = path.as_posix()
    seen.add(key)
    if key.endswith("_ut.cpp"):
        continue
    hits = []
    for match in re.finditer(r"\bmouseGeometry\s*\(", text):
        end = closing(text, match.end() - 1)
        if end is None:
            continue
        arguments = text[match.end():end]
        # Brackets only, and no angle brackets: `terminal->composer` carries
        # a `>` that would take the depth negative and hide the commas after
        # it, which is how the first version of this scan called the four
        # three-argument sites single-argument ones.
        depth = 0
        for char in arguments:
            if char in "([":
                depth += 1
            elif char in ")]":
                depth -= 1
            elif char == "," and depth == 0:
                break
        else:
            hits.append(f"{key}:{text.count(chr(10), 0, match.start()) + 1}")
    if not hits:
        continue
    if key not in allowance:
        bad += hits
    elif len(hits) > allowance[key]:
        bad += hits[allowance[key]:]
if bad:
    sys.stderr.write(
        "mouseGeometry(const Composer&) is the pane that fills the window (A8), "
        "which production no longer gets to assume: pass the pane's origin.\n"
        "Unallowed uses:\n  " + "\n  ".join(bad) + "\n"
    )
    sys.exit(1)
stale = sorted(set(allowance) - seen)
if stale:
    sys.stderr.write(
        "the pointer-geometry audit is allowing files the scan never reached, "
        "so the form it exists to meter now lives somewhere it cannot see: "
        "re-key the allowance onto where these live now.\n"
        "Unreachable:\n  " + "\n  ".join(stale) + "\n"
    )
    sys.exit(1)
""" % (mouse_geometry_allowance, guard_scan_roots, guard_scan_suffixes)

mouse_geometry_guard = untimed_command(
    name="mouse_geometry_guard",
    inputs=["$(S)/build.py", *guard_scan_sources],
    outputs=["$(B)/tst/mouse-geometry-guard.stamp"],
    cmd=[
        ["python3", "-c", mouse_geometry_guard_program],
        touch_stamp("$(B)/tst/mouse-geometry-guard.stamp"),
    ],
    cwd="$(S)",
    descr="MG",
    color="cyan",
)


# A9 (R6-arch, A6-1). Every backend used to take the size of a pane's grid from
# the window: 7 reads in render_metal.mm, 23 in render_reference.cpp, 8 in
# render_vk.cpp, and not one of them legitimately about the window - they were
# grid walks, cell indexing, bounds checks and push constants. The size now
# travels with the data it describes, in TerminalUpdate::gridColumns/gridRows.
#
# There are no allowances and there is no number to grow into. A renderer has no
# business asking the composer for a grid at all: the composer's grid is the one
# the window would have if it held a single pane, which is exactly the
# assumption A9 removed. The window-sized quantities a renderer legitimately
# reads are pixelWidth/pixelHeight, and those are not spelled like this.
#
# Restricted to render*, because everyone else - Composer::resize(), the mouse
# frontend, the test harness - is asking about the window and is right to.
# T6.1 put a number on that "everyone else" rather than leaving it an
# assertion: dropping the restriction red-lines 98 hits across 10 files -
# composer_ut.cpp 31, ui_csd_tabs_ut.cpp 23, vt_headless_ut.cpp and
# ui_sidebar_tabs_ut.cpp 12 each, session_ut.cpp 8, then application.cpp,
# span_shaper.cpp, test_mode.cpp, ui_csd_tabs.mm and application_ut.cpp - and
# every one of them is a window question asked by something that is not a
# renderer. The restriction stays. What it costs is written down instead: it
# selects by filename, and the backends check below only knows three names, so
# a fourth renderer that is not called render* would be read by nothing and
# missed by the self-check too. Naming it render* is therefore load-bearing.
# Scanned through blanked source, so a comment naming the field (this file aside,
# several of them do) is spaces by the time the check reads it.
#
# T2.1 added the second half of this list, and it is the half that matters for
# the merge. Upstream took columns and rows off Composer and put them inside
# VtGeometry, so the window's grid is now spelled composer.geometry.columns and
# the four names above appear nowhere in it. This is not a hypothetical:
# running these four guards against a clean origin/master tree, where
# render_reference.cpp reads composer_.geometry.columns/rows twenty-four times
# - the precise behaviour A9 exists to forbid - returns rc=0 and an empty
# report (T0.3, section 3.1). Adding a root would not have touched it, because
# the renderers never move. The old names stay: they are cheap, and they catch
# code that has not made the crossing yet.
#
# M6 added the third half. T2.1 had guessed the crossing would be spelled
# composer.geometry.columns, from the plan's reading of upstream; the commit
# that actually made it (25dbda61) named the embedding surface VtState and
# spelled the window grid composer.vt.columns instead, and with only the eight
# names above a probe reading composer.vt.columns from render_reference.cpp
# returned rc=0 and an empty report - A9 unguarded behind a green guard, the
# failure T2.1 was written to close, arriving through the spelling nobody had
# yet seen.
#
# M6c (bd86ed38) dissolved VtState, and the window grid is spelled
# composer.geometry.columns again - the spelling T2.1 guessed, arriving one
# merge step later than it expected. Re-measured by probe on the merged tree,
# both ways round: composer.geometry.columns/rows planted in
# render_reference.cpp red-lines on both, and so does the now-dead
# composer.vt.columns. All twelve names stay. The vt.* four cost nothing and
# name a spelling this tree carried for two merge steps; deleting them buys
# only the chance of needing them back.
#
# The list is deliberately qualified rather than a bare "geometry.columns". A
# renderer reading the *pane's* geometry off the update it is drawing is what
# A9 asks for, and the day that field exists a bare name would red-line it.
# What is still open, and is written down rather than guessed at: a renderer
# could bind `const auto& g = composer.geometry;` and read g.columns, and no
# spelling in this list sees that. It is one alias away and worth revisiting if
# it ever appears; it does not appear today, in our tree or upstream's.
pane_grid_names = (
    "composer.columns",
    "composer.rows",
    "composer_.columns",
    "composer_.rows",
    "composer.geometry.columns",
    "composer.geometry.rows",
    "composer_.geometry.columns",
    "composer_.geometry.rows",
    "composer.vt.columns",
    "composer.vt.rows",
    "composer_.vt.columns",
    "composer_.vt.rows",
)

# The three backends A9 is about. A guard that scans no renderer is not a green
# guard, it is an absent one, and the whole failure mode T2.1 exists to close is
# a scan that quietly addresses an empty set - which is what "the renderers
# moved and the roots did not follow" looks like from the outside.
pane_grid_backends = ("render_metal.mm", "render_reference.cpp", "render_vk.cpp")

pane_grid_guard_program = guard_source_reader + r"""
names = %r
backends = set(%r)
bad = []
seen = set()
for path, text in scanned(%r, %r):
    if not path.name.startswith("render"):
        continue
    seen.add(path.name)
    key = path.as_posix()
    for number, line in enumerate(text.splitlines(), 1):
        for name in names:
            bad += [f"{key}:{number}"] * line.count(name)
if bad:
    sys.stderr.write(
        "A renderer takes the grid of the pane it is drawing from the update "
        "that carries its cells (A9: TerminalUpdate::gridColumns/gridRows), "
        "never from the window - the composer's grid is the window with one "
        "pane in it.\n"
        "Unallowed uses:\n  " + "\n  ".join(bad) + "\n"
    )
    sys.exit(1)
missing = sorted(backends - seen)
if missing:
    sys.stderr.write(
        "the renderer grid audit never reached a renderer it is meant to "
        "cover, so it passed by reading nothing: point the scan roots at "
        "where these live now.\n"
        "Unreachable:\n  " + "\n  ".join(missing) + "\n"
    )
    sys.exit(1)
""" % (pane_grid_names, pane_grid_backends, guard_scan_roots, guard_scan_suffixes)

pane_grid_guard = untimed_command(
    name="pane_grid_guard",
    inputs=["$(S)/build.py", *guard_scan_sources],
    outputs=["$(B)/tst/pane-grid-guard.stamp"],
    cmd=[
        ["python3", "-c", pane_grid_guard_program],
        touch_stamp("$(B)/tst/pane-grid-guard.stamp"),
    ],
    cwd="$(S)",
    descr="PG",
    color="cyan",
)


# L1 was an unresolved symbol in every non-Apple build: a call into a
# darwin-only translation unit, from a portable one, outside any #if. It was
# found by an audit written for the occasion - and then written again, from
# scratch, by T5, T6, R4-qa, R5-qa and T7, five times, because it lived in a
# scratchpad and never in the tree. At least two of those five had holes their
# authors found only by controlling them: one was blind to calls that stand
# alone as a statement, the other moved its own line numbers by deleting
# comments. This is that audit, once, wired to a build step.
#
# What it tracks it derives rather than lists: every top-level non-static
# definition in a darwin-only unit (*.mm), intersected with what some header
# declares - a portable unit can only call what it can see - minus anything a
# portable unit also defines. Five symbols today, and the five are exactly the
# doors L1 came through. Listing them by hand would have been a sixth thing to
# keep in step with the tree.
#
# A call is told from a prototype by what stands before the name: a type for a
# prototype, and nothing, a bracket, an operator or a keyword for a call. The
# other way round - requiring an optional type - swallows every call that is a
# statement of its own, which is the hole R5-qa found in its own instrument and
# the shape of one of the two controls in the report.
#
# HAVE_METAL_RENDERER and HAVE_CORETEXT count as darwin conditions: build.py
# defines them in the darwin branch only, and an audit that looked for
# __APPLE__ alone reports render.cpp's Metal call as a false alarm (R2-qa,
# round 4).
darwin_guard_macros = ("__APPLE__", "HAVE_METAL_RENDERER", "HAVE_CORETEXT")

darwin_call_guard_program = guard_source_reader + r"""
macros = %r
keywords = frozenset((
    "return", "if", "while", "for", "switch", "case", "do", "else", "and", "or", "not",
    "sizeof", "new", "delete", "throw", "static_cast", "const_cast", "reinterpret_cast",
))
head = r"(?m)^([A-Za-z_][A-Za-z_0-9:<>,&*\[\] \t]*?)\b([A-Za-z_][A-Za-z_0-9]*)\s*\("


def named(text, terminator):
    found = set()
    for match in re.finditer(head, text):
        if not match.group(1).strip():
            continue
        if terminator == "{" and "static" in match.group(1).split():
            continue
        end = closing(text, match.end() - 1)
        if end is None:
            continue
        rest = re.sub(r"^(const|noexcept|override|final)\s*", "", text[end + 1:end + 200].lstrip())
        if rest.startswith(terminator):
            found.add(match.group(2))
    return found


def darwin(expression):
    return "!" not in expression and any(macro in expression for macro in macros)


def guarded(text):
    state = []
    result = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            directive = stripped[1:].strip()
            word = directive.split(None, 1)[0] if directive else ""
            rest = directive[len(word):].strip()
            if word in ("if", "ifdef", "ifndef"):
                now = rest in macros if word == "ifdef" else (False if word == "ifndef" else darwin(rest))
                otherwise = rest in macros if word == "ifndef" else False
                state.append([now, otherwise])
            elif word == "elif" and state:
                state[-1][0] = darwin(rest)
            elif word == "else" and state:
                state[-1][0] = state[-1][1]
            elif word == "endif" and state:
                state.pop()
        result.append(any(frame[0] for frame in state))
    return result


sources = list(scanned(%r, %r))
darwin_defined = set()
portable_defined = set()
declared = set()
for path, text in sources:
    if path.suffix == ".mm":
        darwin_defined |= named(text, "{")
    else:
        portable_defined |= named(text, "{")
    if path.suffix == ".h":
        declared |= named(text, ";")
tracked = (darwin_defined & declared) - portable_defined

bad = []
for path, text in sources:
    if path.suffix == ".mm":
        continue
    inside = guarded(text)
    for number, line in enumerate(text.splitlines(), 1):
        if inside[number - 1]:
            continue
        for name in sorted(tracked):
            for match in re.finditer(r"\b" + name + r"\s*\(", line):
                before = line[:match.start()].rstrip()
                if before.endswith(("*", "&")):
                    continue
                if re.search(r"[A-Za-z_0-9]$", before) and before.split()[-1] not in keywords:
                    continue
                bad.append(f"{path.as_posix()}:{number}  {name}")
if bad:
    sys.stderr.write(
        "A darwin-only symbol is called where a non-Apple build reaches it, "
        "which is an unresolved symbol on every platform but macOS (R2-test, L1).\n"
        f"Tracked: {' '.join(sorted(tracked))}\n"
        "Unguarded calls:\n  " + "\n  ".join(bad) + "\n"
    )
    sys.exit(1)
if not tracked:
    sys.stderr.write("the darwin call audit tracks nothing at all, which means it stopped working\n")
    sys.exit(1)
""" % (darwin_guard_macros, guard_scan_roots, guard_scan_suffixes)

darwin_call_guard = untimed_command(
    name="darwin_call_guard",
    inputs=["$(S)/build.py", *guard_scan_sources],
    outputs=["$(B)/tst/darwin-call-guard.stamp"],
    cmd=[
        ["python3", "-c", darwin_call_guard_program],
        touch_stamp("$(B)/tst/darwin-call-guard.stamp"),
    ],
    cwd="$(S)",
    descr="DA",
    color="cyan",
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
    inputs=["$(S)/tst/fuzz_parser.py", "$(S)/tst/harness.py"],
    outputs=["$(B)/parser-fuzz.stamp"],
    deps=[st_test],
    cmd=[
        ["python3", "tst/fuzz_parser.py"],
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
    inputs=["$(S)/tst/vttest.py", "$(S)/tst/harness.py"],
    outputs=["$(B)/vttest.stamp"],
    deps=[st_test],
    cmd=[
        ["python3", "tst/vttest.py"],
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


xtermjs_root = Path(__file__).parent / "tst" / "xtermjs"
xtermjs_cases = (xtermjs_root / "file_names.txt").read_text().split()
xtermjs_tests = []
for case in xtermjs_cases:
    xtermjs_tests.append(command(
        name="xtermjs_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/xtermjs/adapter.py",
            "$(S)/tst/xtermjs/file_names.txt",
            "$(S)/tst/xtermjs/xfail.txt",
            f"$(S)/tst/xtermjs/{case}.in",
            f"$(S)/tst/xtermjs/{case}.text",
        ],
        outputs=[f"$(B)/tst/xtermjs/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/xtermjs/adapter.py",
            case,
            "tst/xtermjs/xfail.txt",
            f"$(B)/tst/xtermjs/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="XJ",
        color="cyan",
    ))


alacritty_root = Path(__file__).parent / "tst" / "alacritty"
alacritty_cases = (alacritty_root / "file_names.txt").read_text().split()
alacritty_tests = []
for case in alacritty_cases:
    alacritty_tests.append(command(
        name="alacritty_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/alacritty/adapter.py",
            "$(S)/tst/alacritty/file_names.txt",
            "$(S)/tst/alacritty/xfail.txt",
            f"$(S)/tst/alacritty/{case}/alacritty.recording",
            f"$(S)/tst/alacritty/{case}/config.json",
            f"$(S)/tst/alacritty/{case}/grid.json",
            f"$(S)/tst/alacritty/{case}/size.json",
        ],
        outputs=[f"$(B)/tst/alacritty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/alacritty/adapter.py",
            case,
            "tst/alacritty/xfail.txt",
            f"$(B)/tst/alacritty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="AL",
        color="cyan",
    ))


contour_vttest_sources = [
    source for source in build.glob("$(S)/tst/contour/vttest/*.c")
    if not source.endswith("/vms_io.c")
]
contour_vttest = program(
    name="contour_vttest_helper",
    srcs=contour_vttest_sources,
    cflags=["-Wno-error"],
    cppflags=[
        "-DHAVE_CONFIG_H",
        "-I$(S)/tst/contour/vttest",
    ],
    output="$(B)/tst/contour/vttest",
)


contour_root = Path(__file__).parent / "tst" / "contour"
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
            "$(S)/tst/harness.py",
            "$(S)/tst/contour/adapter.py",
            "$(S)/tst/contour/file_names.txt",
            "$(S)/tst/contour/scenarios.json",
            "$(S)/tst/contour/xfail.txt",
            *golden_inputs,
        ],
        outputs=[f"$(B)/tst/contour/{case}.stamp"],
        deps=[st_test, contour_vttest],
        cmd=[
            "python3",
            "tst/contour/adapter.py",
            "$(B)/tst/contour/vttest",
            case,
            "tst/contour/xfail.txt",
            f"$(B)/tst/contour/{case}.stamp",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/mosh/adapter.py",
            "$(S)/tst/mosh/xfail.txt",
            *build.glob(f"$(S)/tst/mosh/{corpus}/*"),
        ],
        outputs=[f"$(B)/tst/mosh/{corpus}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/mosh/adapter.py",
            corpus,
            "tst/mosh/xfail.txt",
            f"$(B)/tst/mosh/{corpus}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="MO",
        color="cyan",
    ))

mosh_root = Path(__file__).parent / "tst" / "mosh"
mosh_semantic_cases = (
    mosh_root / "semantic_file_names.txt"
).read_text().split()
mosh_semantic_tests = []
for case in mosh_semantic_cases:
    mosh_semantic_tests.append(command(
        name="mosh_semantic_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/mosh/semantic_adapter.py",
            "$(S)/tst/mosh/semantic_cases.py",
            "$(S)/tst/mosh/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tst/mosh/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/mosh/semantic_adapter.py",
            case,
            f"$(B)/tst/mosh/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="MO",
        color="cyan",
    ))

mosh_semantic_validation = command(
    name="mosh_semantic_catalog",
    inputs=[
        "$(S)/tst/mosh/semantic_cases.py",
        "$(S)/tst/mosh/semantic_file_names.txt",
        "$(S)/tst/mosh/semantic_validate.py",
    ],
    outputs=["$(B)/tst/mosh/semantic/catalog.stamp"],
    cmd=[
        ["python3", "tst/mosh/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/mosh/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="MO",
    color="cyan",
)


libtsm_root = Path(__file__).parent / "tst" / "libtsm"
libtsm_semantic_cases = (
    libtsm_root / "semantic_file_names.txt"
).read_text().split()
libtsm_semantic_tests = []
for case in libtsm_semantic_cases:
    libtsm_semantic_tests.append(command(
        name="libtsm_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/libtsm/semantic_adapter.py",
            "$(S)/tst/libtsm/semantic_cases.py",
            "$(S)/tst/libtsm/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tst/libtsm/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/libtsm/semantic_adapter.py",
            case,
            f"$(B)/tst/libtsm/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="LT",
        color="cyan",
    ))

libtsm_semantic_validation = command(
    name="libtsm_semantic_catalog",
    inputs=[
        "$(S)/tst/libtsm/semantic_cases.py",
        "$(S)/tst/libtsm/semantic_file_names.txt",
        "$(S)/tst/libtsm/semantic_validate.py",
    ],
    outputs=["$(B)/tst/libtsm/catalog.stamp"],
    cmd=[
        ["python3", "tst/libtsm/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/libtsm/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="LT",
    color="cyan",
)


ghostty_root = Path(__file__).parent / "tst" / "ghostty"
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
ghostty_shard_size = 64
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
                "$(S)/tst/harness.py",
                "$(S)/tst/fuzz_parser.py",
                "$(S)/tst/ghostty/adapter.py",
                "$(S)/tst/ghostty/xfail.txt",
                *("$(S)/tst/ghostty/" + member for member in shard),
            ],
            outputs=[f"$(B)/tst/ghostty/{name}.stamp"],
            deps=[st_test],
            cmd=[
                "python3",
                "tst/ghostty/adapter.py",
                "tst/ghostty/xfail.txt",
                f"$(B)/tst/ghostty/{name}.stamp",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/ghostty/semantic_adapter.py",
            "$(S)/tst/ghostty/semantic_catalog.py",
            "$(S)/tst/ghostty/semantic_file_names.txt",
            "$(S)/tst/ghostty/semantic_xfail.txt",
            "$(S)/tst/ghostty/upstream/stream_terminal_tests.zig",
        ],
        outputs=[f"$(B)/tst/ghostty/model/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/ghostty/semantic_adapter.py",
            case,
            "tst/ghostty/semantic_xfail.txt",
            f"$(B)/tst/ghostty/model/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="GH",
        color="cyan",
    ))


ghostty_semantic_validation = command(
    name="ghostty_model_catalog",
    inputs=[
        "$(S)/tst/ghostty/semantic_catalog.py",
        "$(S)/tst/ghostty/semantic_file_names.txt",
        "$(S)/tst/ghostty/semantic_validate.py",
        "$(S)/tst/ghostty/semantic_xfail.txt",
        "$(S)/tst/ghostty/upstream/stream_terminal_tests.zig",
    ],
    outputs=["$(B)/tst/ghostty/model/catalog.stamp"],
    cmd=[
        ["python3", "tst/ghostty/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/ghostty/model/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="GH",
    color="cyan",
)


kitty_root = Path(__file__).parent / "tst" / "kitty"
kitty_cases = (kitty_root / "file_names.txt").read_text().split()
kitty_tests = []
for case in kitty_cases:
    kitty_tests.append(command(
        name="kitty_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/kitty/adapter.py",
            "$(S)/tst/kitty/catalog.py",
            "$(S)/tst/kitty/file_names.txt",
            "$(S)/tst/kitty/xfail.txt",
            "$(S)/tst/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tst/kitty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/kitty/adapter.py",
            case,
            "tst/kitty/xfail.txt",
            f"$(B)/tst/kitty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KI",
        color="cyan",
    ))


kitty_validation = command(
    name="kitty_catalog",
    inputs=[
        "$(S)/tst/kitty/catalog.py",
        "$(S)/tst/kitty/file_names.txt",
        "$(S)/tst/kitty/validate.py",
        "$(S)/tst/kitty/xfail.txt",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/catalog.stamp"],
    cmd=[
        ["python3", "tst/kitty/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/kitty/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/kitty/screen_adapter.py",
            "$(S)/tst/kitty/screen_catalog.py",
            "$(S)/tst/kitty/screen_file_names.txt",
            "$(S)/tst/kitty/screen_xfail.txt",
            "$(S)/tst/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tst/kitty/screen/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/kitty/screen_adapter.py",
            case,
            "tst/kitty/screen_xfail.txt",
            f"$(B)/tst/kitty/screen/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KS",
        color="cyan",
    ))


kitty_screen_validation = command(
    name="kitty_screen_catalog",
    inputs=[
        "$(S)/tst/kitty/screen_catalog.py",
        "$(S)/tst/kitty/screen_file_names.txt",
        "$(S)/tst/kitty/screen_validate.py",
        "$(S)/tst/kitty/screen_xfail.txt",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/screen/catalog.stamp"],
    cmd=[
        ["python3", "tst/kitty/screen_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/kitty/screen/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KS",
    color="cyan",
)


kitty_utf8 = command(
    name="kitty_utf8",
    inputs=[
        "$(S)/tst/harness.py",
        "$(S)/tst/kitty/utf8_adapter.py",
        "$(S)/tst/kitty/utf8_catalog.py",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/utf8.stamp"],
    deps=[st_test],
    cmd=[
        "python3",
        "tst/kitty/utf8_adapter.py",
        "$(B)/tst/kitty/utf8.stamp",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/kitty/transaction_adapter.py",
            "$(S)/tst/kitty/transaction_cases.py",
            "$(S)/tst/kitty/transaction_file_names.txt",
            "$(S)/tst/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tst/kitty/transaction/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/kitty/transaction_adapter.py",
            case,
            f"$(B)/tst/kitty/transaction/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KT",
        color="cyan",
    ))

kitty_transaction_validation = command(
    name="kitty_transaction_catalog",
    inputs=[
        "$(S)/tst/harness.py",
        "$(S)/tst/kitty/transaction_cases.py",
        "$(S)/tst/kitty/transaction_file_names.txt",
        "$(S)/tst/kitty/transaction_validate.py",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/transaction/catalog.stamp"],
    cmd=[
        ["python3", "tst/kitty/transaction_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/kitty/transaction/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KT",
    color="cyan",
)


vte_root = Path(__file__).parent / "tst" / "vte"
vte_cases = (vte_root / "file_names.txt").read_text().split()
vte_tests = []
for case in vte_cases:
    vte_tests.append(command(
        name="vte_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/adapter.py",
            "$(S)/tst/vte/catalog.py",
            "$(S)/tst/vte/file_names.txt",
            "$(S)/tst/vte/xfail.txt",
            "$(S)/tst/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/adapter.py",
            case,
            "tst/vte/xfail.txt",
            f"$(B)/tst/vte/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        test_timeout_seconds=120,
        descr="VE",
        color="cyan",
    ))


vte_validation = command(
    name="vte_catalog",
    inputs=[
        "$(S)/tst/vte/catalog.py",
        "$(S)/tst/vte/file_names.txt",
        "$(S)/tst/vte/validate.py",
        "$(S)/tst/vte/xfail.txt",
        "$(S)/tst/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tst/vte/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VE",
    color="cyan",
)


vte_known_cases = (vte_root / "known_file_names.txt").read_text().split()
vte_known_tests = []
for case in vte_known_cases:
    vte_known_tests.append(command(
        name="vte_known_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/known_adapter.py",
            "$(S)/tst/vte/known_cases.py",
            "$(S)/tst/vte/known_file_names.txt",
            *build.glob("$(S)/tst/vte/upstream/parser-*.hh"),
            "$(S)/tst/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/known/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/known_adapter.py",
            case,
            f"$(B)/tst/vte/known/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        test_timeout_seconds=120,
        descr="VK",
        color="cyan",
    ))


vte_known_validation = command(
    name="vte_known_catalog",
    inputs=[
        "$(S)/tst/vte/known_cases.py",
        "$(S)/tst/vte/known_file_names.txt",
        "$(S)/tst/vte/known_validate.py",
        "$(S)/tst/vte/upstream/parser-esc.hh",
        "$(S)/tst/vte/upstream/parser-csi.hh",
        "$(S)/tst/vte/upstream/parser-dcs.hh",
        "$(S)/tst/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tst/vte/known/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/known_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/known/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/charset_adapter.py",
            "$(S)/tst/vte/charset_cases.py",
            "$(S)/tst/vte/charset_file_names.txt",
            "$(S)/tst/vte/upstream/parser-charset-tables.hh",
            "$(S)/tst/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/charset/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/charset_adapter.py",
            case,
            f"$(B)/tst/vte/charset/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VC",
        color="cyan",
    ))


vte_charset_validation = command(
    name="vte_charset_catalog",
    inputs=[
        "$(S)/tst/vte/charset_cases.py",
        "$(S)/tst/vte/charset_file_names.txt",
        "$(S)/tst/vte/charset_validate.py",
        "$(S)/tst/vte/upstream/parser-charset-tables.hh",
        "$(S)/tst/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tst/vte/charset/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/charset_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/charset/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/tabstop_adapter.py",
            "$(S)/tst/vte/tabstop_cases.py",
            "$(S)/tst/vte/tabstop_file_names.txt",
            "$(S)/tst/vte/upstream/tabstops-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/tabstops/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/tabstop_adapter.py",
            case,
            f"$(B)/tst/vte/tabstops/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VT",
        color="cyan",
    ))


vte_tabstop_validation = command(
    name="vte_tabstop_catalog",
    inputs=[
        "$(S)/tst/vte/tabstop_cases.py",
        "$(S)/tst/vte/tabstop_file_names.txt",
        "$(S)/tst/vte/tabstop_validate.py",
        "$(S)/tst/vte/upstream/tabstops-test.cc",
    ],
    outputs=["$(B)/tst/vte/tabstops/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/tabstop_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/tabstops/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/mode_adapter.py",
            "$(S)/tst/vte/mode_cases.py",
            "$(S)/tst/vte/mode_file_names.txt",
            "$(S)/tst/vte/upstream/modes-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/modes/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/mode_adapter.py",
            case,
            f"$(B)/tst/vte/modes/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VM",
        color="cyan",
    ))


vte_mode_validation = command(
    name="vte_mode_catalog",
    inputs=[
        "$(S)/tst/vte/mode_cases.py",
        "$(S)/tst/vte/mode_file_names.txt",
        "$(S)/tst/vte/mode_validate.py",
        "$(S)/tst/vte/upstream/modes-test.cc",
    ],
    outputs=["$(B)/tst/vte/modes/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/mode_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/modes/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/color_adapter.py",
            "$(S)/tst/vte/color_cases.py",
            "$(S)/tst/vte/color_file_names.txt",
            "$(S)/tst/vte/upstream/color-test.cc",
            "$(S)/tst/vte/upstream/color-names-tests.hh",
        ],
        outputs=[f"$(B)/tst/vte/color/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/color_adapter.py",
            case,
            f"$(B)/tst/vte/color/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VC",
        color="cyan",
    ))


vte_color_validation = command(
    name="vte_color_catalog",
    inputs=[
        "$(S)/tst/vte/color_cases.py",
        "$(S)/tst/vte/color_file_names.txt",
        "$(S)/tst/vte/color_validate.py",
        "$(S)/tst/vte/upstream/color-test.cc",
        "$(S)/tst/vte/upstream/color-names-tests.hh",
    ],
    outputs=["$(B)/tst/vte/color/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/color_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/color/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/paste_adapter.py",
            "$(S)/tst/vte/paste_cases.py",
            "$(S)/tst/vte/paste_file_names.txt",
            "$(S)/tst/vte/upstream/pastify-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/paste/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/paste_adapter.py",
            case,
            f"$(B)/tst/vte/paste/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VP",
        color="cyan",
    ))


vte_paste_validation = command(
    name="vte_paste_catalog",
    inputs=[
        "$(S)/tst/vte/paste_cases.py",
        "$(S)/tst/vte/paste_file_names.txt",
        "$(S)/tst/vte/paste_validate.py",
        "$(S)/tst/vte/upstream/pastify-test.cc",
    ],
    outputs=["$(B)/tst/vte/paste/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/paste_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/paste/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/utf8_adapter.py",
            "$(S)/tst/vte/utf8_cases.py",
            "$(S)/tst/vte/utf8_file_names.txt",
        ],
        outputs=[f"$(B)/tst/vte/utf8/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/utf8_adapter.py",
            case,
            f"$(B)/tst/vte/utf8/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VU",
        color="cyan",
    ))


vte_utf8_validation = command(
    name="vte_utf8_catalog",
    inputs=[
        "$(S)/tst/vte/utf8_cases.py",
        "$(S)/tst/vte/utf8_file_names.txt",
        "$(S)/tst/vte/utf8_validate.py",
    ],
    outputs=["$(B)/tst/vte/utf8/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/utf8_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/utf8/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/width_adapter.py",
            "$(S)/tst/vte/width_catalog.py",
            "$(S)/tst/vte/width_file_names.txt",
            "$(S)/tst/vte/width_xfail.txt",
            "$(S)/tst/vte/upstream/unicode-width-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/width/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vte/width_adapter.py",
            case,
            "tst/vte/width_xfail.txt",
            f"$(B)/tst/vte/width/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VW",
        color="cyan",
    ))


vte_width_validation = command(
    name="vte_width_catalog",
    inputs=[
        "$(S)/tst/vte/width_catalog.py",
        "$(S)/tst/vte/width_file_names.txt",
        "$(S)/tst/vte/width_validate.py",
        "$(S)/tst/vte/width_xfail.txt",
        "$(S)/tst/vte/upstream/unicode-width-test.cc",
    ],
    outputs=["$(B)/tst/vte/width/catalog.stamp"],
    cmd=[
        ["python3", "tst/vte/width_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/width/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VW",
    color="cyan",
)


windows_terminal_root = Path(__file__).parent / "tst" / "windows_terminal"
windows_terminal_cases = (windows_terminal_root / "file_names.txt").read_text().split()
windows_terminal_tests = []
for case in windows_terminal_cases:
    windows_terminal_tests.append(command(
        name="windows_terminal_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/windows_terminal/adapter.py",
            "$(S)/tst/windows_terminal/catalog.py",
            "$(S)/tst/windows_terminal/file_names.txt",
            "$(S)/tst/windows_terminal/xfail.txt",
            "$(S)/tst/windows_terminal/upstream/StateMachineTest.cpp",
            "$(S)/tst/windows_terminal/upstream/OutputEngineTest.cpp",
        ],
        outputs=[f"$(B)/tst/windows_terminal/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/windows_terminal/adapter.py",
            case,
            "tst/windows_terminal/xfail.txt",
            f"$(B)/tst/windows_terminal/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WT",
        color="cyan",
    ))


windows_terminal_validation = command(
    name="windows_terminal_catalog",
    inputs=[
        "$(S)/tst/windows_terminal/catalog.py",
        "$(S)/tst/windows_terminal/file_names.txt",
        "$(S)/tst/windows_terminal/validate.py",
        "$(S)/tst/windows_terminal/xfail.txt",
        "$(S)/tst/windows_terminal/upstream/StateMachineTest.cpp",
        "$(S)/tst/windows_terminal/upstream/OutputEngineTest.cpp",
    ],
    outputs=["$(B)/tst/windows_terminal/catalog.stamp"],
    cmd=[
        ["python3", "tst/windows_terminal/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/windows_terminal/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WT",
    color="cyan",
)


wezterm_root = Path(__file__).parent / "tst" / "wezterm"
wezterm_cases = (wezterm_root / "file_names.txt").read_text().split()
wezterm_tests = []
for case in wezterm_cases:
    wezterm_tests.append(command(
        name="wezterm_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/wezterm/adapter.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/file_names.txt",
            "$(S)/tst/wezterm/xfail.txt",
            *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tst/wezterm/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/adapter.py",
            case,
            "tst/wezterm/xfail.txt",
            f"$(B)/tst/wezterm/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WZ",
        color="cyan",
    ))


wezterm_validation = command(
    name="wezterm_catalog",
    inputs=[
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/file_names.txt",
        "$(S)/tst/wezterm/validate.py",
        "$(S)/tst/wezterm/xfail.txt",
        *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tst/wezterm/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/screen_adapter.py",
            "$(S)/tst/wezterm/screen_catalog.py",
            "$(S)/tst/wezterm/screen_file_names.txt",
            "$(S)/tst/wezterm/screen_xfail.txt",
            *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tst/wezterm/screen/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/screen_adapter.py",
            case,
            "tst/wezterm/screen_xfail.txt",
            f"$(B)/tst/wezterm/screen/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WS",
        color="cyan",
    ))


wezterm_screen_validation = command(
    name="wezterm_screen_catalog",
    inputs=[
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        "$(S)/tst/wezterm/screen_file_names.txt",
        "$(S)/tst/wezterm/screen_validate.py",
        "$(S)/tst/wezterm/screen_xfail.txt",
        *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tst/wezterm/screen/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/screen_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/screen/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/upstream/selection.rs",
            "$(S)/tst/wezterm/selection_adapter.py",
            "$(S)/tst/wezterm/selection_cases.py",
            "$(S)/tst/wezterm/selection_file_names.txt",
            "$(S)/tst/wezterm/selection_xfail.txt",
        ],
        outputs=[f"$(B)/tst/wezterm/selection/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/selection_adapter.py",
            case,
            "tst/wezterm/selection_xfail.txt",
            f"$(B)/tst/wezterm/selection/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WQ",
        color="cyan",
    ))

wezterm_selection_validation = command(
    name="wezterm_selection_catalog",
    inputs=[
        "$(S)/tst/wezterm/upstream/selection.rs",
        "$(S)/tst/wezterm/selection_cases.py",
        "$(S)/tst/wezterm/selection_file_names.txt",
        "$(S)/tst/wezterm/selection_xfail.txt",
        "$(S)/tst/wezterm/selection_validate.py",
    ],
    outputs=["$(B)/tst/wezterm/selection/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/selection_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/selection/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/cursor_adapter.py",
            "$(S)/tst/wezterm/cursor_cases.py",
            "$(S)/tst/wezterm/cursor_file_names.txt",
            "$(S)/tst/wezterm/screen_catalog.py",
            *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tst/wezterm/cursor/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/cursor_adapter.py",
            case,
            f"$(B)/tst/wezterm/cursor/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WC",
        color="cyan",
    ))

wezterm_cursor_validation = command(
    name="wezterm_cursor_catalog",
    inputs=[
        "$(S)/tst/wezterm/cursor_cases.py",
        "$(S)/tst/wezterm/cursor_file_names.txt",
        "$(S)/tst/wezterm/cursor_validate.py",
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tst/wezterm/cursor/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/cursor_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/cursor/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/damage_adapter.py",
            "$(S)/tst/wezterm/damage_cases.py",
            "$(S)/tst/wezterm/damage_file_names.txt",
            "$(S)/tst/wezterm/screen_catalog.py",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/damage/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/damage_adapter.py",
            case,
            f"$(B)/tst/wezterm/damage/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WD",
        color="cyan",
    ))

wezterm_damage_validation = command(
    name="wezterm_damage_catalog",
    inputs=[
        "$(S)/tst/wezterm/damage_cases.py",
        "$(S)/tst/wezterm/damage_file_names.txt",
        "$(S)/tst/wezterm/damage_validate.py",
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/damage/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/damage_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/damage/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/history_adapter.py",
            "$(S)/tst/wezterm/history_cases.py",
            "$(S)/tst/wezterm/history_file_names.txt",
            "$(S)/tst/wezterm/screen_catalog.py",
            "$(S)/tst/wezterm/upstream/csi.rs",
            "$(S)/tst/wezterm/upstream/mod.rs",
            "$(S)/tst/wezterm/upstream/selection.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/history/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/history_adapter.py",
            case,
            f"$(B)/tst/wezterm/history/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WH",
        color="cyan",
    ))

wezterm_history_validation = command(
    name="wezterm_history_catalog",
    inputs=[
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/history_cases.py",
        "$(S)/tst/wezterm/history_file_names.txt",
        "$(S)/tst/wezterm/history_validate.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        "$(S)/tst/wezterm/upstream/csi.rs",
        "$(S)/tst/wezterm/upstream/mod.rs",
        "$(S)/tst/wezterm/upstream/selection.rs",
    ],
    outputs=["$(B)/tst/wezterm/history/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/history_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/history/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/semantic_adapter.py",
            "$(S)/tst/wezterm/semantic_cases.py",
            "$(S)/tst/wezterm/semantic_file_names.txt",
            "$(S)/tst/wezterm/semantic_xfail.txt",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/semantic_adapter.py",
            case,
            "tst/wezterm/semantic_xfail.txt",
            f"$(B)/tst/wezterm/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WM",
        color="cyan",
    ))

wezterm_semantic_validation = command(
    name="wezterm_semantic_catalog",
    inputs=[
        "$(S)/tst/wezterm/semantic_cases.py",
        "$(S)/tst/wezterm/semantic_file_names.txt",
        "$(S)/tst/wezterm/semantic_validate.py",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/semantic/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/semantic/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/hyperlink_adapter.py",
            "$(S)/tst/wezterm/hyperlink_cases.py",
            "$(S)/tst/wezterm/hyperlink_file_names.txt",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/hyperlink/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/hyperlink_adapter.py",
            case,
            f"$(B)/tst/wezterm/hyperlink/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WL",
        color="cyan",
    ))

wezterm_hyperlink_validation = command(
    name="wezterm_hyperlink_catalog",
    inputs=[
        "$(S)/tst/wezterm/hyperlink_cases.py",
        "$(S)/tst/wezterm/hyperlink_file_names.txt",
        "$(S)/tst/wezterm/hyperlink_validate.py",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/hyperlink/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/hyperlink_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/hyperlink/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/metadata_adapter.py",
            "$(S)/tst/wezterm/metadata_cases.py",
            "$(S)/tst/wezterm/metadata_file_names.txt",
            "$(S)/tst/wezterm/upstream/csi.rs",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/metadata/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/wezterm/metadata_adapter.py",
            case,
            f"$(B)/tst/wezterm/metadata/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WX",
        color="cyan",
    ))

wezterm_metadata_validation = command(
    name="wezterm_metadata_catalog",
    inputs=[
        "$(S)/tst/wezterm/metadata_cases.py",
        "$(S)/tst/wezterm/metadata_file_names.txt",
        "$(S)/tst/wezterm/metadata_validate.py",
        "$(S)/tst/wezterm/upstream/csi.rs",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/metadata/catalog.stamp"],
    cmd=[
        ["python3", "tst/wezterm/metadata_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/metadata/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WX",
    color="cyan",
)


konsole_root = Path(__file__).parent / "tst" / "konsole"
konsole_cases = (konsole_root / "file_names.txt").read_text().split()
konsole_tests = []
for case in konsole_cases:
    konsole_tests.append(command(
        name="konsole_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/konsole/adapter.py",
            "$(S)/tst/konsole/catalog.py",
            "$(S)/tst/konsole/file_names.txt",
            "$(S)/tst/konsole/xfail.txt",
            "$(S)/tst/konsole/upstream/Vt102EmulationTest.cpp",
        ],
        outputs=[f"$(B)/tst/konsole/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/konsole/adapter.py",
            case,
            "tst/konsole/xfail.txt",
            f"$(B)/tst/konsole/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KO",
        color="cyan",
    ))


konsole_validation = command(
    name="konsole_catalog",
    inputs=[
        "$(S)/tst/konsole/catalog.py",
        "$(S)/tst/konsole/file_names.txt",
        "$(S)/tst/konsole/validate.py",
        "$(S)/tst/konsole/xfail.txt",
        "$(S)/tst/konsole/upstream/Vt102EmulationTest.cpp",
    ],
    outputs=["$(B)/tst/konsole/catalog.stamp"],
    cmd=[
        ["python3", "tst/konsole/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/semantic_adapter.py",
            "$(S)/tst/konsole/semantic_cases.py",
            "$(S)/tst/konsole/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/konsole/semantic_adapter.py",
            case,
            f"$(B)/tst/konsole/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KS",
        color="cyan",
    ))

konsole_semantic_validation = command(
    name="konsole_semantic_catalog",
    inputs=[
        "$(S)/tst/konsole/semantic_cases.py",
        "$(S)/tst/konsole/semantic_file_names.txt",
        "$(S)/tst/konsole/semantic_validate.py",
    ],
    outputs=["$(B)/tst/konsole/semantic/catalog.stamp"],
    cmd=[
        ["python3", "tst/konsole/semantic_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/semantic/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/vt_adapter.py",
            "$(S)/tst/konsole/vt_cases.py",
            "$(S)/tst/konsole/vt_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/vt/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/konsole/vt_adapter.py",
            case,
            f"$(B)/tst/konsole/vt/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KV",
        color="cyan",
    ))

konsole_vt_validation = command(
    name="konsole_vt_catalog",
    inputs=[
        "$(S)/tst/konsole/vt_cases.py",
        "$(S)/tst/konsole/vt_file_names.txt",
        "$(S)/tst/konsole/vt_validate.py",
    ],
    outputs=["$(B)/tst/konsole/vt/catalog.stamp"],
    cmd=[
        ["python3", "tst/konsole/vt_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/vt/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/upstream/CharacterWidthTest.cpp",
            "$(S)/tst/konsole/width_adapter.py",
            "$(S)/tst/konsole/width_catalog.py",
            "$(S)/tst/konsole/width_file_names.txt",
            "$(S)/tst/ucd.py",
            "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
            "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
        ],
        outputs=[f"$(B)/tst/konsole/width/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/konsole/width_adapter.py",
            case,
            f"$(B)/tst/konsole/width/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KW",
        color="cyan",
    ))

konsole_width_validation = command(
    name="konsole_width_catalog",
    inputs=[
        "$(S)/tst/konsole/upstream/CharacterWidthTest.cpp",
        "$(S)/tst/konsole/width_catalog.py",
        "$(S)/tst/konsole/width_file_names.txt",
        "$(S)/tst/konsole/width_validate.py",
        "$(S)/tst/ucd.py",
        "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
        "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
    ],
    outputs=["$(B)/tst/konsole/width/catalog.stamp"],
    cmd=[
        ["python3", "tst/konsole/width_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/width/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/upstream/KeyboardTranslatorTest.cpp",
            "$(S)/tst/konsole/keyboard_adapter.py",
            "$(S)/tst/konsole/keyboard_catalog.py",
            "$(S)/tst/konsole/keyboard_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/keyboard/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/konsole/keyboard_adapter.py",
            case,
            f"$(B)/tst/konsole/keyboard/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KK",
        color="cyan",
    ))

konsole_keyboard_validation = command(
    name="konsole_keyboard_catalog",
    inputs=[
        "$(S)/tst/konsole/upstream/KeyboardTranslatorTest.cpp",
        "$(S)/tst/konsole/keyboard_catalog.py",
        "$(S)/tst/konsole/keyboard_file_names.txt",
        "$(S)/tst/konsole/keyboard_validate.py",
    ],
    outputs=["$(B)/tst/konsole/keyboard/catalog.stamp"],
    cmd=[
        ["python3", "tst/konsole/keyboard_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/keyboard/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/upstream/PtyTest.cpp",
            "$(S)/tst/konsole/pty_adapter.py",
            "$(S)/tst/konsole/pty_catalog.py",
            "$(S)/tst/konsole/pty_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/pty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/konsole/pty_adapter.py",
            case,
            f"$(B)/tst/konsole/pty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KP",
        color="cyan",
    ))

konsole_pty_validation = command(
    name="konsole_pty_catalog",
    inputs=[
        "$(S)/tst/konsole/upstream/PtyTest.cpp",
        "$(S)/tst/konsole/pty_catalog.py",
        "$(S)/tst/konsole/pty_file_names.txt",
        "$(S)/tst/konsole/pty_validate.py",
    ],
    outputs=["$(B)/tst/konsole/pty/catalog.stamp"],
    cmd=[
        ["python3", "tst/konsole/pty_validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/pty/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KP",
    color="cyan",
)


tmux_root = Path(__file__).parent / "tst" / "tmux"
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
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/tmux/adapter.py",
            "$(S)/tst/tmux/xfail.txt",
            *("$(S)/tst/tmux/" + member for member in shard),
        ],
        outputs=[f"$(B)/tst/tmux/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/tmux/adapter.py",
            "tst/tmux/xfail.txt",
            f"$(B)/tst/tmux/{name}.stamp",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/tmux/adapter.py",
            "$(S)/tst/tmux/xfail.txt",
            "$(S)/tst/tmux/upstream/input-fuzzer.dict",
        ],
        outputs=[f"$(B)/tst/tmux/input_dictionary_{index}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/tmux/adapter.py",
            "tst/tmux/xfail.txt",
            f"$(B)/tst/tmux/input_dictionary_{index}.stamp",
            member,
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TM",
        color="cyan",
    ))


wraptest_helper = program(
    name="wraptest_helper",
    srcs=["$(S)/tst/wraptest/wraptest.c"],
    cflags=["-Wno-error"],
    output="$(B)/tst/wraptest/wraptest",
)

wraptest_root = Path(__file__).parent / "tst" / "wraptest"
wraptest_cases = json.loads((wraptest_root / "cases.json").read_text())
wraptest_tests = []
for case_id, _, _ in wraptest_cases:
    wraptest_tests.append(command(
        name="wraptest_" + case_id,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wraptest/adapter.py",
            "$(S)/tst/wraptest/cases.json",
            "$(S)/tst/wraptest/xfail.txt",
        ],
        outputs=[f"$(B)/tst/wraptest/{case_id}.stamp"],
        deps=[st_test, wraptest_helper],
        cmd=[
            "python3",
            "tst/wraptest/adapter.py",
            "$(B)/tst/wraptest/wraptest",
            case_id,
            "tst/wraptest/xfail.txt",
            f"$(B)/tst/wraptest/{case_id}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WR",
        color="cyan",
    ))


tack_root = Path(__file__).parent / "tst" / "tack"
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
tack_upstream_inputs = [
    *build.glob("$(S)/tst/tack/upstream/*"),
    "$(S)/tst/tack/upstream/.clang-format",
]
tack_program = command(
    name="tack_program",
    inputs=[
        "$(S)/tst/tack/build_tack.sh",
        *tack_upstream_inputs,
    ],
    outputs=["$(B)/tst/tack/tack"],
    cmd=[
        "sh",
        "tst/tack/build_tack.sh",
        "tst/tack/upstream",
        "$(B)/tst/tack/tack",
    ],
    cwd="$(S)",
    descr="TC",
    color="magenta",
)
tack_validation = command(
    name="tack_catalog",
    inputs=[
        "$(S)/tst/tack/file_names.txt",
        "$(S)/tst/tack/validate.py",
        *tack_upstream_inputs,
    ],
    outputs=["$(B)/tst/tack/catalog.stamp"],
    cmd=[
        ["python3", "tst/tack/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/tack/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/tack/adapter.py",
            "$(S)/tst/tack/file_names.txt",
            "$(S)/tst/tack/xfail.txt",
        ],
        outputs=[f"$(B)/tst/tack/{capability}.stamp"],
        deps=[st_test, tack_program],
        cmd=[
            "python3",
            "tst/tack/adapter.py",
            "$(B)/tst/tack/tack",
            capability,
            "tst/tack/xfail.txt",
            f"$(B)/tst/tack/{capability}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TA",
        color="cyan",
    ))


ucs_detect_root = Path(__file__).parent / "tst" / "ucs_detect"
ucs_detect_tests = []
ucs_detect_table_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted(ucs_detect_root.glob("table_*.py"))
]
# The catalog derives the skipped visible format controls from the vendored
# UCD through tst/ucd.py, so every case list depends on these files.
ucs_detect_table_inputs += [
    "$(S)/tst/ucd.py",
    "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
    "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/ucs_detect/adapter.py",
            "$(S)/tst/ucs_detect/catalog.py",
            "$(S)/tst/ucs_detect/shards.txt",
            "$(S)/tst/ucs_detect/xfail.txt",
            *ucs_detect_table_inputs,
        ],
        outputs=[f"$(B)/tst/ucs_detect/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/ucs_detect/adapter.py",
            category,
            str(start),
            str(end),
            "tst/ucs_detect/xfail.txt",
            f"$(B)/tst/ucs_detect/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="UC",
        color="cyan",
    ))

ucs_detect_validation = command(
    name="ucs_detect_catalog",
    inputs=[
        "$(S)/tst/ucs_detect/catalog.py",
        "$(S)/tst/ucs_detect/probe_cases.py",
        "$(S)/tst/ucs_detect/probe_names.txt",
        "$(S)/tst/ucs_detect/probe_xfail.txt",
        "$(S)/tst/ucs_detect/validate.py",
        "$(S)/tst/ucs_detect/shards.txt",
        "$(S)/tst/ucs_detect/xfail.txt",
        *ucs_detect_table_inputs,
    ],
    outputs=["$(B)/tst/ucs_detect/catalog.stamp"],
    cmd=[
        ["python3", "tst/ucs_detect/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/ucs_detect/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/ucs_detect/probe_adapter.py",
            "$(S)/tst/ucs_detect/probe_cases.py",
            "$(S)/tst/ucs_detect/probe_names.txt",
            "$(S)/tst/ucs_detect/probe_xfail.txt",
            "$(S)/tst/ucs_detect/upstream/terminal.py",
            "$(S)/tst/ucs_detect/upstream/table_xtgettcap.py",
            "$(S)/tst/ucs_detect/upstream/data/shitty.yaml",
        ],
        outputs=[f"$(B)/tst/ucs_detect/probes/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/ucs_detect/probe_adapter.py",
            case,
            "tst/ucs_detect/probe_xfail.txt",
            f"$(B)/tst/ucs_detect/probes/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="UP",
        color="cyan",
    ))


vtebench_root = Path(__file__).parent / "tst" / "vtebench"
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
            "$(S)/tst/harness.py",
            "$(S)/tst/vtebench/adapter.py",
            "$(S)/tst/vtebench/file_names.txt",
            "$(S)/tst/vtebench/xfail.txt",
            *vtebench_case_inputs,
        ],
        outputs=[f"$(B)/tst/vtebench/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/vtebench/adapter.py",
            case,
            "tst/vtebench/xfail.txt",
            f"$(B)/tst/vtebench/{case}.stamp",
            "30",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VB",
        color="yellow",
    ))


libvterm_root = Path(__file__).parent / "tst" / "libvterm"
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
            "$(S)/tst/harness.py",
            "$(S)/tst/libvterm/adapter.py",
            "$(S)/tst/libvterm/file_names.txt",
            "$(S)/tst/libvterm/xfail.txt",
            f"$(S)/tst/libvterm/upstream/{case}",
        ],
        outputs=[f"$(B)/tst/libvterm/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/libvterm/adapter.py",
            case,
            "tst/libvterm/xfail.txt",
            f"$(B)/tst/libvterm/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="LV",
        color="cyan",
    ))


xterm_vttests_root = Path(__file__).parent / "tst" / "xterm_vttests"
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
            "$(S)/tst/harness.py",
            "$(S)/tst/xterm_vttests/adapter.py",
            "$(S)/tst/xterm_vttests/file_names.txt",
            "$(S)/tst/xterm_vttests/xfail.txt",
            *build.glob("$(S)/tst/xterm_vttests/bin/*"),
            *build.glob("$(S)/tst/xterm_vttests/lib/**/*.pm"),
            *build.glob("$(S)/tst/xterm_vttests/upstream/*"),
        ],
        outputs=[f"$(B)/tst/xterm_vttests/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/xterm_vttests/adapter.py",
            case,
            "tst/xterm_vttests/xfail.txt",
            f"$(B)/tst/xterm_vttests/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="XV",
        color="cyan",
    ))


esctest_root = Path(__file__).parent / "tst" / "esctest"
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
            "$(S)/tst/harness.py",
            "$(S)/tst/esctest/adapter.py",
            "$(S)/tst/esctest/file_names.txt",
            "$(S)/tst/esctest/xfail.txt",
            *esctest_ported_inputs,
        ],
        outputs=[f"$(B)/tst/esctest/{name}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/esctest/adapter.py",
            case,
            "tst/esctest/xfail.txt",
            f"$(B)/tst/esctest/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        test_timeout_seconds=120,
        descr="ES",
        color="cyan",
    ))


termless_root = Path(__file__).parent / "tst" / "termless"
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
        "$(S)/tst/termless/cases.json",
        "$(S)/tst/termless/validate.py",
        "$(S)/tst/termless/xfail.txt",
        *termless_upstream_inputs,
    ],
    outputs=["$(B)/tst/termless/catalog.stamp"],
    cmd=[
        ["python3", "tst/termless/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/termless/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/termless/adapter.py",
            "$(S)/tst/termless/backend.py",
            "$(S)/tst/termless/cases.py",
            "$(S)/tst/termless/cases.json",
            "$(S)/tst/termless/xfail.txt",
            *termless_upstream_inputs,
        ],
        outputs=[f"$(B)/tst/termless/{case_id}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/termless/adapter.py",
            case_id,
            "tst/termless/xfail.txt",
            f"$(B)/tst/termless/{case_id}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TL",
        color="cyan",
    ))


realworld_root = Path(__file__).parent / "tst" / "realworld"
realworld_cases = (realworld_root / "file_names.txt").read_text().split()
realworld_validation = command(
    name="realworld_catalog",
    inputs=[
        "$(S)/tst/realworld/validate.py",
        "$(S)/tst/realworld/corpus.py",
        "$(S)/tst/realworld/zstd_codec.py",
        "$(S)/tst/realworld/cases.json",
        "$(S)/tst/realworld/file_names.txt",
        *build.glob("$(S)/tst/realworld/input/*.input.zst"),
        *build.glob("$(S)/tst/realworld/screen/*.screen.json"),
    ],
    outputs=["$(B)/tst/realworld/catalog.stamp"],
    cmd=[
        ["python3", "tst/realworld/validate.py"],
        [
            "python3", "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/realworld/catalog.stamp').touch()",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/realworld/adapter.py",
            "$(S)/tst/realworld/corpus.py",
            "$(S)/tst/realworld/zstd_codec.py",
            "$(S)/tst/realworld/cases.json",
            "$(S)/tst/realworld/file_names.txt",
            f"$(S)/tst/realworld/input/{case}.input.zst",
            f"$(S)/tst/realworld/screen/{case}.screen.json",
        ],
        outputs=[f"$(B)/tst/realworld/{case}.stamp"],
        deps=[st_test],
        cmd=[
            "python3",
            "tst/realworld/adapter.py",
            case,
            f"$(B)/tst/realworld/{case}.stamp",
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
            "$(S)/tst/harness.py",
            "$(S)/tst/keyboard_layout_product.py",
        ],
        outputs=[output],
        deps=[st_test],
        cmd=[
            [
                "python3",
                "tst/keyboard_layout_product.py",
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


group("install", st, pt)


# The lib/vterm boundary: the core includes nothing from lib/shitty.
vterm_boundary = command(
    name="vterm_boundary",
    inputs=[
        "$(S)/lib/vterm/check_includes.py",
        *build.glob("$(S)/lib/vterm/*.h"),
        *build.glob("$(S)/lib/vterm/*.cpp"),
    ],
    outputs=["$(B)/vterm-boundary.stamp"],
    cmd=[
        "python3",
        "$(S)/lib/vterm/check_includes.py",
        "$(S)/lib/vterm",
        "$(B)/vterm-boundary.stamp",
    ],
    descr="VB",
    color="magenta",
)

add_test(production_surface, pretty_binary_branding, vterm_boundary, border_pixels_guard, mouse_geometry_guard, pane_grid_guard, darwin_call_guard, instrumented=False)
if linux:
    add_test(wayland_frame_stall, wayland_title_fallback, instrumented=False)
# The perf programs link no test of their own, so nothing else builds them:
# they went uncompilable for a whole merge without CI noticing (T5.11).
add_test(parser_perf, core_perf, instrumented=False)

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
