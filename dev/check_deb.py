#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import hashlib
import io
import os
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path

from build_deb import (
    PACKAGE_ARCHITECTURE,
    PACKAGE_NAME,
    clean_build_environment,
    derive_dependencies,
    run,
)

PAYLOAD_MODES = {
    "usr/bin/st": 0o755,
    "usr/share/applications/shitty.desktop": 0o644,
    "usr/share/icons/hicolor/scalable/apps/shitty.svg": 0o644,
    "usr/share/doc/shitty/README.md": 0o644,
    "usr/share/doc/shitty/LICENSING": 0o644,
    "usr/share/doc/shitty/SOURCE": 0o644,
    "usr/share/doc/shitty/copyright": 0o644,
    "usr/share/doc/shitty/LICENSE.MIT": 0o644,
    "usr/share/doc/shitty/LICENSE.OFL": 0o644,
    "usr/share/doc/shitty/LICENSE.NotoColorEmoji": 0o644,
    "usr/share/doc/shitty/changelog.gz": 0o644,
}


def field(package: Path, name: str) -> str:
    return run(["dpkg-deb", "--field", os.fspath(package), name], capture=True)


def check_payload(package: Path) -> None:
    archive_data = subprocess.check_output(["dpkg-deb", "--fsys-tarfile", package])
    with tarfile.open(fileobj=io.BytesIO(archive_data), mode="r:") as archive:
        regular = {}
        for member in archive.getmembers():
            normalized = member.name.removeprefix("./")
            if member.uid != 0 or member.gid != 0:
                raise RuntimeError(f"payload member is not root-owned: {member.name}")
            if member.isdir() and member.mode & 0o7777 != 0o755:
                raise RuntimeError(
                    f"payload directory has unexpected mode: {member.name}"
                )
            if member.isfile():
                regular[normalized] = member.mode & 0o7777
        if regular != PAYLOAD_MODES:
            raise RuntimeError(f"unexpected payload files or modes: {regular!r}")


def check_md5sums(package: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shitty-deb-control-") as control_name:
        control = Path(control_name)
        run(["dpkg-deb", "--control", os.fspath(package), os.fspath(control)])
        expected = {}
        for line in (control / "md5sums").read_text().splitlines():
            digest, relative = line.split(maxsplit=1)
            expected[relative] = digest
    with tempfile.TemporaryDirectory(prefix="shitty-deb-md5-") as payload_name:
        payload = Path(payload_name)
        run(["dpkg-deb", "--extract", os.fspath(package), os.fspath(payload)])
        actual = {
            relative: hashlib.md5((payload / relative).read_bytes()).hexdigest()
            for relative in expected
        }
    if actual != expected:
        raise RuntimeError("payload does not match DEBIAN/md5sums")


def check_extracted_binary(package: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shitty-deb-check-") as temporary_name:
        root = Path(temporary_name)
        run(["dpkg-deb", "--extract", os.fspath(package), os.fspath(root)])
        binary = root / "usr/bin/st"
        environment = clean_build_environment(0)
        description = run(
            ["file", os.fspath(binary)], environment=environment, capture=True
        )
        if not all(
            marker in description for marker in ("ELF 64-bit", "x86-64", "executable")
        ):
            raise RuntimeError(f"unexpected packaged binary: {description}")
        source = (root / "usr/share/doc/shitty/SOURCE").read_text()
        match = re.fullmatch(
            r"commit ([0-9a-f]{40})\nbinary-version ([0-9]{4}\.[0-9]{2}\.[0-9]{2})\n",
            source,
        )
        if match is None:
            raise RuntimeError(f"unexpected SOURCE metadata: {source!r}")
        sha, binary_version = match.groups()
        notes = run(
            ["readelf", "-n", os.fspath(binary)], environment=environment, capture=True
        )
        if not re.search(rf"^\s*Build ID: {sha}$", notes, re.MULTILINE):
            raise RuntimeError(
                f"packaged binary build ID does not match source commit {sha}"
            )
        dynamic = run(
            ["readelf", "-d", os.fspath(binary)], environment=environment, capture=True
        )
        if "RPATH" in dynamic or "RUNPATH" in dynamic:
            raise RuntimeError("packaged binary contains an RPATH or RUNPATH")
        dependencies = derive_dependencies(binary, environment)
        if field(package, "Depends") != dependencies:
            raise RuntimeError("declared Depends do not match dpkg-shlibdeps")
        linked = run(["ldd", os.fspath(binary)], environment=environment, capture=True)
        if "not found" in linked:
            raise RuntimeError(f"unresolved runtime library:\n{linked}")
        reported_version = run(
            [os.fspath(binary), "-v"], environment=environment, capture=True
        )
        if not reported_version.startswith(f"Shitty {binary_version}\n"):
            raise RuntimeError(f"unexpected version output: {reported_version!r}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a Shitty Ubuntu package without installing it."
    )
    parser.add_argument("package", type=Path)
    arguments = parser.parse_args()
    package = arguments.package.resolve(strict=True)

    run(["dpkg-deb", "--info", os.fspath(package)])
    run(["dpkg-deb", "--contents", os.fspath(package)])
    if field(package, "Package") != PACKAGE_NAME:
        raise RuntimeError("unexpected package name")
    if field(package, "Architecture") != PACKAGE_ARCHITECTURE:
        raise RuntimeError("unexpected package architecture")
    version = field(package, "Version")
    run(["dpkg", "--validate-version", version])
    check_payload(package)
    check_md5sums(package)
    check_extracted_binary(package)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
