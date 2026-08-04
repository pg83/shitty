#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import gzip
import hashlib
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path

PACKAGE_NAME = "shitty"
PACKAGE_ARCHITECTURE = "amd64"
BUILD_ENVIRONMENT_VARIABLES = (
    "CFLAGS",
    "CPPFLAGS",
    "CXXFLAGS",
    "LDFLAGS",
    "LD_LIBRARY_PATH",
    "LIBRARY_PATH",
    "CPATH",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_LIBDIR",
)


def run(
    arguments: list[str],
    *,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
    capture: bool = False,
) -> str:
    location = f" (in {cwd})" if cwd is not None else ""
    print(f"+ {shlex.join(arguments)}{location}", file=sys.stderr)
    result = subprocess.run(
        arguments,
        cwd=cwd,
        env=environment,
        check=True,
        stdout=subprocess.PIPE if capture else None,
        text=True,
    )
    return result.stdout.strip() if capture else ""


def verify_tools() -> None:
    tools = (
        "clang",
        "clang++",
        "dpkg",
        "dpkg-deb",
        "dpkg-shlibdeps",
        "git",
        "glslangValidator",
        "pkg-config",
        "ragel",
        "rsvg-convert",
        "strip",
        "wayland-scanner",
    )
    for tool in tools:
        if not shutil.which(tool):
            raise RuntimeError(f"required tool is not available: {tool}")


def git_value(project_root: Path, format_string: str) -> str:
    return run(
        ["git", "show", "-s", f"--format={format_string}", "HEAD"],
        cwd=project_root,
        capture=True,
    )


def verify_source_tree(project_root: Path) -> None:
    status = run(
        ["git", "status", "--porcelain", "--untracked-files=normal"],
        cwd=project_root,
        capture=True,
    )
    if status:
        raise RuntimeError("refusing to package a dirty source tree")


def source_version(timestamp: int) -> str:
    return datetime.fromtimestamp(timestamp, UTC).strftime("%Y.%m.%d")


def package_version(project_root: Path, timestamp: int, release_tag: str | None) -> str:
    if release_tag is not None:
        if (
            not release_tag.isascii()
            or not release_tag.isdecimal()
            or release_tag.startswith("0")
        ):
            raise RuntimeError(
                "release tag must be a positive decimal integer without leading zeroes"
            )
        version = release_tag
    else:
        revision = git_value(project_root, "%h")
        date = datetime.fromtimestamp(timestamp, UTC).strftime("%Y%m%d")
        version = f"0~git{date}.{revision}"
    run(["dpkg", "--validate-version", version])
    return version


def clean_build_environment(
    timestamp: int,
    sha: str | None = None,
) -> dict[str, str]:
    environment = os.environ.copy()
    for variable in BUILD_ENVIRONMENT_VARIABLES:
        environment.pop(variable, None)
    build_id = "sha1" if sha is None else f"0x{sha}"
    environment.update(
        {
            "CC": "clang",
            "CXX": "clang++",
            "LDFLAGS": f"-Wl,--as-needed,--build-id={build_id}",
            "SHITTY_VERSION": source_version(timestamp),
            "SOURCE_DATE_EPOCH": str(timestamp),
        }
    )
    environment["CPPFLAGS"] = shlex.join(
        [
            "-ffile-prefix-map=$(S)=/usr/src/shitty",
            "-ffile-prefix-map=$(B)=/usr/src/shitty/.build",
        ]
    )
    return environment


def derive_dependencies(binary: Path, environment: dict[str, str]) -> str:
    with tempfile.TemporaryDirectory(prefix="shitty-shlibdeps-") as temporary_name:
        temporary = Path(temporary_name)
        debian = temporary / "debian"
        debian.mkdir()
        (debian / "control").write_text(
            f"Source: {PACKAGE_NAME}\n"
            "Section: x11\n"
            "Priority: optional\n"
            "Maintainer: Shitty team <noreply@github.com>\n"
            "\n"
            f"Package: {PACKAGE_NAME}\n"
            f"Architecture: {PACKAGE_ARCHITECTURE}\n"
            "Description: dependency scan\n"
        )
        installed_binary = debian / PACKAGE_NAME / "usr/bin/st"
        installed_binary.parent.mkdir(parents=True)
        shutil.copyfile(binary, installed_binary)
        output = run(
            ["dpkg-shlibdeps", "-O", f"-e{installed_binary.relative_to(temporary)}"],
            cwd=temporary,
            environment=environment,
            capture=True,
        )
    prefix = "shlibs:Depends="
    lines = [
        line.removeprefix(prefix)
        for line in output.splitlines()
        if line.startswith(prefix)
    ]
    if len(lines) != 1 or not lines[0]:
        raise RuntimeError(f"dpkg-shlibdeps returned unexpected output: {output!r}")
    return lines[0]


def copy_payload(project_root: Path, package_root: Path, binary: Path) -> None:
    files = {
        binary: package_root / "usr/bin/st",
        project_root / "shitty.desktop": package_root
        / "usr/share/applications/shitty.desktop",
        project_root / "shitty.svg": package_root
        / "usr/share/icons/hicolor/scalable/apps/shitty.svg",
        project_root / "README.md": package_root / "usr/share/doc/shitty/README.md",
        project_root / "LICENSE": package_root / "usr/share/doc/shitty/LICENSING",
        project_root / "LICENSE.MIT": package_root / "usr/share/doc/shitty/LICENSE.MIT",
        project_root / "fonts/OFL.txt": package_root
        / "usr/share/doc/shitty/LICENSE.OFL",
        project_root / "fonts/LICENSE.NotoColorEmoji": package_root
        / "usr/share/doc/shitty/LICENSE.NotoColorEmoji",
    }
    for source, destination in files.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(
            0o755 if destination == package_root / "usr/bin/st" else 0o644
        )


def write_copyright(package_root: Path) -> None:
    contents = """Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Shitty
Source: https://github.com/pg83/shitty

Files: *
Copyright: 2020 Tom Szilagyi
           2026 Shitty team
License: GPL-3+
 The imported baseline remains licensed under the GNU General Public License,
 version 3 or any later version. Shitty contributions are also available under
 the MIT license; see /usr/share/doc/shitty/LICENSING and LICENSE.MIT.
 .
 On Debian systems, the complete text of the GNU General Public License,
 version 3, is available in /usr/share/common-licenses/GPL-3.

Files: fonts/JetBrainsMonoNerdFont-Regular.ttf
Copyright: 2020 The JetBrains Mono Project Authors
           Nerd Fonts project contributors
License: OFL-1.1
 See /usr/share/doc/shitty/LICENSE.OFL.

Files: fonts/NotoColorEmoji.ttf fonts/NotoEmoji-Regular.ttf
Copyright: 2013 Google LLC
License: OFL-1.1
 See /usr/share/doc/shitty/LICENSE.NotoColorEmoji.
"""
    output = package_root / "usr/share/doc/shitty/copyright"
    output.write_text(contents)
    output.chmod(0o644)


def write_source(package_root: Path, sha: str, binary_version: str) -> None:
    output = package_root / "usr/share/doc/shitty/SOURCE"
    output.write_text(f"commit {sha}\nbinary-version {binary_version}\n")
    output.chmod(0o644)


def write_changelog(package_root: Path, version: str, timestamp: int) -> None:
    date = datetime.fromtimestamp(timestamp, UTC).strftime(
        "%a, %d %b %Y %H:%M:%S +0000"
    )
    contents = (
        f"shitty ({version}) unstable; urgency=medium\n\n"
        "  * Build the upstream Shitty release.\n\n"
        f" -- Shitty team <noreply@github.com>  {date}\n"
    ).encode()
    output = package_root / "usr/share/doc/shitty/changelog.gz"
    with (
        output.open("wb") as output_file,
        gzip.GzipFile(
            filename="",
            mode="wb",
            fileobj=output_file,
            compresslevel=9,
            mtime=timestamp,
        ) as archive,
    ):
        archive.write(contents)
    output.chmod(0o644)


def installed_size(package_root: Path) -> int:
    size = sum(
        item.stat().st_size for item in package_root.rglob("*") if item.is_file()
    )
    return max(1, (size + 1023) // 1024)


def write_control(package_root: Path, version: str, dependencies: str) -> None:
    control_directory = package_root / "DEBIAN"
    control_directory.mkdir(mode=0o755)
    control = (
        f"Package: {PACKAGE_NAME}\n"
        f"Version: {version}\n"
        f"Architecture: {PACKAGE_ARCHITECTURE}\n"
        "Maintainer: Shitty team <noreply@github.com>\n"
        "Section: x11\n"
        "Priority: optional\n"
        f"Installed-Size: {installed_size(package_root)}\n"
        f"Depends: {dependencies}\n"
        "Conflicts: stterm\n"
        "Replaces: stterm\n"
        "Homepage: https://github.com/pg83/shitty\n"
        "Description: fast Wayland and Vulkan terminal emulator\n"
        " Shitty is a low-latency terminal emulator with a native Wayland\n"
        " frontend, Vulkan rendering, Unicode grapheme support, and embedded\n"
        " fallback fonts. This package targets Ubuntu 24.04 amd64.\n"
    )
    (control_directory / "control").write_text(control)


def write_md5sums(package_root: Path) -> None:
    entries = []
    for item in sorted(package_root.rglob("*")):
        if item.is_file() and "DEBIAN" not in item.relative_to(package_root).parts:
            digest = hashlib.md5(item.read_bytes()).hexdigest()
            entries.append(f"{digest}  {item.relative_to(package_root).as_posix()}")
    output = package_root / "DEBIAN/md5sums"
    output.write_text("\n".join(entries) + "\n")
    output.chmod(0o644)


def normalize_timestamps(package_root: Path, timestamp: int) -> None:
    items = [package_root, *sorted(package_root.rglob("*"))]
    for item in items:
        if item.is_dir():
            item.chmod(0o755)
        os.utime(item, (timestamp, timestamp), follow_symlinks=False)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build an Ubuntu 24.04 amd64 Shitty package."
    )
    parser.add_argument(
        "--release-tag", help="numeric GitHub release tag used as the Debian revision"
    )
    parser.add_argument("--build-directory", type=Path, default=Path(".build-deb"))
    parser.add_argument("--output-directory", type=Path, default=Path("dist"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    arguments = parser.parse_args()
    if arguments.jobs < 1:
        parser.error("--jobs must be positive")

    verify_tools()
    project_root = Path(__file__).resolve().parent.parent
    verify_source_tree(project_root)
    architecture = run(["dpkg", "--print-architecture"], capture=True)
    if architecture != PACKAGE_ARCHITECTURE:
        raise RuntimeError(
            f"package builder requires {PACKAGE_ARCHITECTURE}, got {architecture}"
        )
    timestamp = int(git_value(project_root, "%ct"))
    sha = git_value(project_root, "%H")
    version = package_version(project_root, timestamp, arguments.release_tag)
    build_directory = arguments.build_directory
    if not build_directory.is_absolute():
        build_directory = project_root / build_directory
    output_directory = arguments.output_directory
    if not output_directory.is_absolute():
        output_directory = project_root / output_directory
    output_directory.mkdir(parents=True, exist_ok=True)
    environment = clean_build_environment(timestamp, sha)

    run(
        [
            os.fspath(project_root / "build"),
            "-B",
            os.fspath(build_directory),
            "-j",
            str(arguments.jobs),
            "st",
        ],
        cwd=project_root,
        environment=environment,
    )
    binary = build_directory / "st"
    if not binary.is_file():
        raise RuntimeError(f"build did not produce {binary}")

    with tempfile.TemporaryDirectory(prefix="shitty-deb-root-") as temporary_name:
        package_root = Path(temporary_name)
        copy_payload(project_root, package_root, binary)
        packaged_binary = package_root / "usr/bin/st"
        run(
            ["strip", "--strip-unneeded", os.fspath(packaged_binary)],
            environment=environment,
        )
        dependencies = derive_dependencies(packaged_binary, environment)
        write_copyright(package_root)
        write_source(package_root, sha, source_version(timestamp))
        write_changelog(package_root, version, timestamp)
        write_control(package_root, version, dependencies)
        write_md5sums(package_root)
        normalize_timestamps(package_root, timestamp)
        filename_version = version.replace(":", "%3a")
        output = (
            output_directory
            / f"{PACKAGE_NAME}_{filename_version}_{PACKAGE_ARCHITECTURE}.deb"
        )
        if output.exists():
            raise RuntimeError(f"refusing to overwrite existing package: {output}")
        run(
            [
                "dpkg-deb",
                "--root-owner-group",
                "--build",
                "--uniform-compression",
                "--threads-max=1",
                "-Zxz",
                "-z9",
                os.fspath(package_root),
                os.fspath(output),
            ],
            environment=environment,
        )
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
