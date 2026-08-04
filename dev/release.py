#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import gzip
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from datetime import UTC, datetime
from pathlib import Path


def run(
    arguments: list[str],
    *,
    cwd: Path | None = None,
    capture: bool = False,
) -> str:
    location = f" (in {cwd})" if cwd is not None else ""
    print(f"+ {shlex.join(arguments)}{location}", file=sys.stderr)
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE if capture else None,
        text=True,
    )
    return result.stdout.strip() if capture else ""


def verify_tools(*, github: bool = False, debian: bool = False) -> None:
    tools = ["git", "file"]
    if github:
        tools.append("gh")
    if debian:
        tools.append("dpkg-deb")
    for tool in tools:
        if not shutil.which(tool):
            raise RuntimeError(f"required tool is not available: {tool}")


def release_tags(repository: str) -> list[int]:
    releases = json.loads(
        run(
            [
                "gh",
                "release",
                "list",
                "--repo",
                repository,
                "--limit",
                "1000",
                "--json",
                "tagName",
            ],
            capture=True,
        )
    )
    return [
        int(release["tagName"])
        for release in releases
        if re.fullmatch(r"[1-9][0-9]*", release["tagName"])
    ]


def release_exists(repository: str, tag: str) -> bool:
    return (
        subprocess.run(
            ["gh", "release", "view", tag, "--repo", repository],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode
        == 0
    )


def remote_refs(remote: str, tag: str) -> dict[str, str]:
    output = run(
        [
            "git",
            "ls-remote",
            remote,
            f"refs/heads/{tag}",
            f"refs/tags/{tag}",
            f"refs/tags/{tag}^{{}}",
        ],
        capture=True,
    )
    refs = {}
    for line in output.splitlines():
        sha, name = line.split()
        refs[name] = sha
    return refs


def verify_remote_refs(refs: dict[str, str], tag: str, sha: str) -> tuple[bool, bool]:
    branch_name = f"refs/heads/{tag}"
    tag_name = f"refs/tags/{tag}"
    peeled_tag_name = f"{tag_name}^{{}}"
    branch_exists = branch_name in refs
    tag_exists = tag_name in refs
    if branch_exists and refs[branch_name] != sha:
        raise RuntimeError(
            f"remote branch {tag} points to {refs[branch_name]}, not {sha}"
        )
    tag_commit = refs.get(peeled_tag_name, refs.get(tag_name))
    if tag_commit is not None and tag_commit != sha:
        raise RuntimeError(f"remote tag {tag} points to {tag_commit}, not {sha}")
    return branch_exists, tag_exists


def tar_info(
    archive: tarfile.TarFile,
    path: Path,
    archive_name: str,
    timestamp: int,
) -> tarfile.TarInfo:
    info = archive.gettarinfo(path, archive_name)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = timestamp
    return info


def write_tar(
    output: Path,
    timestamp: int,
    write_contents,
) -> None:
    with (
        output.open("wb") as compressed_file,
        gzip.GzipFile(
            filename="",
            mode="wb",
            fileobj=compressed_file,
            compresslevel=9,
            mtime=timestamp,
        ) as gzip_file,
        tarfile.open(
            fileobj=gzip_file,
            mode="w",
            format=tarfile.GNU_FORMAT,
        ) as archive,
    ):
        write_contents(archive)


def create_source_archive(
    checkout: Path,
    output: Path,
    prefix: str,
    timestamp: int,
) -> None:
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z"],
        cwd=checkout,
    ).split(b"\0")
    paths = sorted(Path(os.fsdecode(name)) for name in tracked if name)

    def write_contents(archive: tarfile.TarFile) -> None:
        root = tarfile.TarInfo(f"{prefix}/")
        root.type = tarfile.DIRTYPE
        root.mode = 0o755
        root.uid = 0
        root.gid = 0
        root.mtime = timestamp
        archive.addfile(root)
        for relative in paths:
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError(f"unsafe tracked path: {relative}")
            source = checkout / relative
            info = tar_info(
                archive,
                source,
                f"{prefix}/{relative.as_posix()}",
                timestamp,
            )
            if info.isreg():
                with source.open("rb") as input_file:
                    archive.addfile(info, input_file)
            else:
                archive.addfile(info)

    write_tar(output, timestamp, write_contents)


def create_binary_archive(
    binary: Path,
    output: Path,
    timestamp: int,
) -> None:
    def write_contents(archive: tarfile.TarFile) -> None:
        info = tar_info(archive, binary, "st", timestamp)
        info.mode = 0o755
        with binary.open("rb") as input_file:
            archive.addfile(info, input_file)

    write_tar(output, timestamp, write_contents)


def resolve_release_tag(repository: str, requested_tag: str | None) -> str:
    numeric_tags = release_tags(repository)
    expected_tag = max(numeric_tags, default=0) + 1
    tag = requested_tag or str(expected_tag)
    if release_exists(repository, tag):
        raise RuntimeError(f"release {tag} already exists")
    if int(tag) != expected_tag:
        raise RuntimeError(f"next release tag is {expected_tag}, not {tag}")
    return tag


def checkout_commit(remote: str, sha: str, destination: Path) -> str:
    run(["git", "clone", "--no-checkout", remote, os.fspath(destination)])
    resolved_sha = run(
        ["git", "rev-parse", "--verify", f"{sha}^{{commit}}"],
        cwd=destination,
        capture=True,
    )
    run(["git", "checkout", "--detach", resolved_sha], cwd=destination)
    return resolved_sha


def build_release_artifacts(
    remote: str,
    sha: str,
    tag: str,
    builder: str,
    artifacts: Path,
) -> list[Path]:
    artifacts.mkdir(parents=True, exist_ok=True)
    source_archive = artifacts / f"shitty-{tag}.tar.gz"
    binary_archive = artifacts / "st-darwin-arm64.tar.gz"
    for output in (source_archive, binary_archive):
        if output.exists():
            raise RuntimeError(f"refusing to overwrite existing artifact: {output}")
    with tempfile.TemporaryDirectory(
        prefix=f"shitty-release-build-{tag}-"
    ) as temporary_name:
        checkout = Path(temporary_name) / "checkout"
        resolved_sha = checkout_commit(remote, sha, checkout)
        timestamp = int(
            run(
                ["git", "show", "-s", "--format=%ct", resolved_sha],
                cwd=checkout,
                capture=True,
            )
        )
        create_source_archive(checkout, source_archive, f"shitty-{tag}", timestamp)
        builder_script = (
            "build_ix_macos.sh" if builder == "ix" else "build_brew_macos.sh"
        )
        run([os.fspath(checkout / "dev" / builder_script)], cwd=checkout)
        binary = checkout / ".build-darwin" / "st"
        if not binary.is_file():
            raise RuntimeError(f"Darwin build did not produce {binary}")
        binary = binary.resolve(strict=True)
        file_description = run(["file", os.fspath(binary)], capture=True)
        if not all(
            marker in file_description
            for marker in ("Mach-O 64-bit", "arm64", "executable")
        ):
            raise RuntimeError(f"unexpected Darwin artifact: {file_description}")
        create_binary_archive(binary, binary_archive, timestamp)
    return [source_archive, binary_archive]


def verify_release_artifacts(
    artifacts: list[Path],
    tag: str,
    sha: str,
    timestamp: int,
) -> list[Path]:
    resolved = [artifact.resolve(strict=True) for artifact in artifacts]
    by_name = {artifact.name: artifact for artifact in resolved}
    if len(by_name) != len(resolved):
        raise RuntimeError("release artifacts must have unique filenames")
    debs = [
        artifact
        for artifact in resolved
        if artifact.name.startswith("shitty_") and artifact.name.endswith("_amd64.deb")
    ]
    if len(debs) != 1:
        raise RuntimeError("release requires exactly one shitty_*_amd64.deb artifact")
    actual_version = run(
        ["dpkg-deb", "--field", os.fspath(debs[0]), "Version"],
        capture=True,
    )
    if actual_version != tag:
        raise RuntimeError(
            f"Debian artifact version is {actual_version}, expected {tag}"
        )
    expected_binary_version = datetime.fromtimestamp(timestamp, UTC).strftime(
        "%Y.%m.%d"
    )
    with tempfile.TemporaryDirectory(prefix="shitty-release-deb-") as temporary_name:
        root = Path(temporary_name)
        run(["dpkg-deb", "--extract", os.fspath(debs[0]), os.fspath(root)])
        source = (root / "usr/share/doc/shitty/SOURCE").read_text()
    expected_source = f"commit {sha}\nbinary-version {expected_binary_version}\n"
    if source != expected_source:
        raise RuntimeError(
            f"Debian artifact SOURCE metadata is {source!r}, expected {expected_source!r}"
        )
    expected_names = {
        f"shitty-{tag}.tar.gz",
        "st-darwin-arm64.tar.gz",
        debs[0].name,
    }
    if set(by_name) != expected_names:
        raise RuntimeError(f"unexpected release artifact set: {sorted(by_name)}")
    return [by_name[name] for name in sorted(expected_names)]


def publish_release(
    remote: str,
    repository: str,
    sha: str,
    tag: str,
    artifacts: list[Path],
    notes: str,
    *,
    generate_notes: bool,
    draft: bool,
) -> str:
    with tempfile.TemporaryDirectory(
        prefix=f"shitty-release-publish-{tag}-"
    ) as temporary_name:
        temporary = Path(temporary_name)
        checkout = temporary / "checkout"
        resolved_sha = checkout_commit(remote, sha, checkout)
        timestamp = int(
            run(
                ["git", "show", "-s", "--format=%ct", resolved_sha],
                cwd=checkout,
                capture=True,
            )
        )
        verified_artifacts = verify_release_artifacts(
            artifacts,
            tag,
            resolved_sha,
            timestamp,
        )
        notes_file = temporary / "release-notes.md"
        if not generate_notes:
            notes_file.write_text(f"{notes}\n")

        refs = remote_refs(remote, tag)
        branch_exists, tag_exists = verify_remote_refs(refs, tag, resolved_sha)
        if not tag_exists:
            run(
                ["git", "tag", "-a", tag, resolved_sha, "-m", f"Release {tag}"],
                cwd=checkout,
            )
        refspecs = []
        if not branch_exists:
            refspecs.append(f"{resolved_sha}:refs/heads/{tag}")
        if not tag_exists:
            refspecs.append(f"refs/tags/{tag}:refs/tags/{tag}")
        if refspecs:
            run(["git", "push", "--atomic", "origin", *refspecs], cwd=checkout)

        run(
            [
                "gh",
                "release",
                "create",
                tag,
                *[os.fspath(artifact) for artifact in verified_artifacts],
                "--repo",
                repository,
                "--verify-tag",
                "--title",
                tag,
                *(
                    ["--generate-notes"]
                    if generate_notes
                    else ["--notes-file", os.fspath(notes_file)]
                ),
                *(["--draft"] if draft else []),
            ],
            cwd=checkout,
        )
        return run(
            [
                "gh",
                "release",
                "view",
                tag,
                "--repo",
                repository,
                "--json",
                "url",
                "--jq",
                ".url",
            ],
            capture=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build and publish a Shitty GitHub release.",
    )
    parser.add_argument(
        "tag", nargs="?", help="numeric release tag; omitted picks the next"
    )
    parser.add_argument("--sha", default="HEAD", help="git commit to release")
    parser.add_argument(
        "--builder",
        choices=("ix", "brew"),
        default="ix",
        help="darwin build environment: the local ix toolchain or the CI brew one",
    )
    parser.add_argument(
        "--generate-notes",
        action="store_true",
        help="let GitHub generate the release notes instead of reading stdin",
    )
    parser.add_argument(
        "--artifacts-directory",
        type=Path,
        help="directory in which to retain the generated release artifacts",
    )
    parser.add_argument(
        "--draft",
        action="store_true",
        help="create a draft release instead of publishing it",
    )
    parser.add_argument(
        "--tag-file",
        type=Path,
        help="write the resolved tag to this file after creating the release",
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--resolve-tag-only",
        action="store_true",
        help="print the next validated release tag without building or publishing",
    )
    mode.add_argument(
        "--build-only",
        action="store_true",
        help="build source and Darwin artifacts without creating refs or a release",
    )
    mode.add_argument(
        "--publish-only",
        action="store_true",
        help="publish prebuilt artifacts without rebuilding them",
    )
    parser.add_argument(
        "--artifact",
        action="append",
        default=[],
        type=Path,
        help="prebuilt artifact to attach in --publish-only mode; repeat for each file",
    )
    arguments = parser.parse_args()

    if arguments.tag is not None and not re.fullmatch(r"[1-9][0-9]*", arguments.tag):
        parser.error("tag must be a positive decimal integer without leading zeroes")

    if arguments.build_only and (
        arguments.tag is None or arguments.artifacts_directory is None
    ):
        parser.error("--build-only requires an explicit tag and --artifacts-directory")
    if arguments.publish_only and not arguments.artifact:
        parser.error("--publish-only requires --artifact")
    if arguments.artifact and not arguments.publish_only:
        parser.error("--artifact is only valid with --publish-only")

    verify_tools(
        github=not arguments.build_only,
        debian=arguments.publish_only,
    )

    project_root = Path(__file__).resolve().parent.parent
    remote = run(
        ["git", "remote", "get-url", "origin"],
        cwd=project_root,
        capture=True,
    )
    local_sha = run(
        ["git", "rev-parse", "--verify", f"{arguments.sha}^{{commit}}"],
        cwd=project_root,
        capture=True,
    )
    if arguments.build_only:
        build_release_artifacts(
            remote,
            local_sha,
            arguments.tag,
            arguments.builder,
            arguments.artifacts_directory.resolve(),
        )
        return 0

    repository = run(
        ["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"],
        cwd=project_root,
        capture=True,
    )
    tag = resolve_release_tag(repository, arguments.tag)
    if arguments.resolve_tag_only:
        print(tag)
        return 0

    notes = "" if arguments.generate_notes else sys.stdin.read().strip()
    if not notes and not arguments.generate_notes:
        parser.error("release notes must be provided on stdin")

    url = publish_release(
        remote,
        repository,
        local_sha,
        tag,
        arguments.artifact,
        notes,
        generate_notes=arguments.generate_notes,
        draft=arguments.draft,
    )
    if arguments.tag_file is not None:
        arguments.tag_file.write_text(f"{tag}\n")
    print(url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
