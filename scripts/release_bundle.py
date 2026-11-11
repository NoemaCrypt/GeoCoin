#!/usr/bin/env python3
"""Stage and validate the explicit file lists of GeoCoin release bundles."""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def manifest(root=ROOT):
    data = json.loads((root / "config/release-bundles.json").read_text(encoding="utf-8"))
    if data["version"] != 1:
        raise ValueError("Unsupported release bundle manifest version")
    return data


def relative_path(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError(f"Invalid bundle path: {name}")
    return path


def stage(root, output, build, profile, platform):
    data = manifest(root)
    spec = data["profiles"][profile]
    extension = ".exe" if platform == "windows" else ""
    copies = {**data["common_files"], **spec["files"]}
    # Check every required input before writing the payload.
    binaries = []
    for name in spec["required_binaries"] + spec["optional_binaries"]:
        source = build / (name + extension)
        if not source.is_file():
            if name in spec["required_binaries"]:
                raise ValueError(f"Required release binary is missing: {source}")
            continue
        binaries.append((source, "bin/" + source.name))
    sources = [(root / relative_path(src), str(relative_path(dst))) for src, dst in copies.items()]
    for source, _ in sources:
        if not source.is_file():
            raise ValueError(f"Required release file is missing: {source}")
    output.mkdir(parents=True, exist_ok=True)
    for source, destination in sources + binaries:
        target = output / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def entries(source):
    """Read metadata only: archive validation never extracts untrusted paths."""
    if source.is_dir():
        result = {}
        for path in source.rglob("*"):
            if path.is_symlink():
                raise ValueError(f"Unexpected symbolic link: {path.relative_to(source)}")
            if path.is_file():
                result[path.relative_to(source).as_posix()] = path.stat().st_mode
        return result
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            rows = [(item.filename, item.external_attr >> 16) for item in archive.infolist() if not item.is_dir()]
    else:
        with tarfile.open(source) as archive:
            members = archive.getmembers()
            if any(not (item.isdir() or item.isfile()) for item in members):
                raise ValueError("Bundle must not contain links or special files")
            files = [item for item in members if item.isfile()]
            # Linux tarballs have exactly one top-level bundle directory.
            roots = {relative_path(item.name).parts[0] for item in files}
            if len(roots) != 1 or any(len(relative_path(item.name).parts) < 2 for item in files):
                raise ValueError("Expected one top-level directory in the tarball")
            rows = [(PurePosixPath(item.name).relative_to(next(iter(roots))).as_posix(), item.mode) for item in files]
    result = {}
    for name, mode in rows:
        relative_path(name)
        if name in result or stat.S_ISLNK(mode):
            raise ValueError(f"Duplicate entry or link: {name}")
        result[name] = mode
    return result


def check(source, profile, platform, root=ROOT):
    data = manifest(root)
    spec = data["profiles"][profile]
    files = entries(source)
    allowed = set(data["common_files"].values()) | set(spec["files"].values())
    required = set(allowed)
    extension = ".exe" if platform == "windows" else ""
    for name in spec["required_binaries"] + spec["optional_binaries"]:
        binary = "bin/" + name + extension
        allowed.add(binary)
        if platform == "linux":
            allowed.add(name)
        if name in spec["required_binaries"]:
            required.add(binary)
            if platform == "linux":
                required.add(name)
    if platform == "windows" and "geocoin-node" in spec["required_binaries"]:
        allowed.add("geocoin-node.cmd")
        required.add("geocoin-node.cmd")
    if profile == "user":
        allowed.add("bin/qt.conf")
        required.add("bin/qt.conf")
    if profile == "sdk":
        if platform == "linux":
            allowed.add("lib/libgeo.so")
            required.add("lib/libgeo.so")
        else:
            allowed.update({"lib/geo.lib", "bin/geo.dll"})
            required.update({"lib/geo.lib", "bin/geo.dll"})
    missing = required - files.keys()
    if missing:
        raise ValueError("Required bundle files missing: " + ", ".join(sorted(missing)))
    for name, mode in files.items():
        relative_path(name)
        runtime = bool(re.fullmatch(r"lib/lib[^/]+\.so(?:\.[0-9]+)*", name)) if platform == "linux" else bool(re.fullmatch(r"bin/[^/]+\.dll", name, re.I))
        if profile != "sdk" and name in {"lib/libgeo.so", "bin/geo.dll"}:
            runtime = False
        if profile != "user" and re.match(r"(?:lib/lib|bin/)Qt[56]", name, re.I):
            runtime = False
        plugins = profile == "user" and bool(re.fullmatch(
            r"plugins/(platforms|imageformats|iconengines|styles)/[^/]+\.so" if platform == "linux"
            else r"bin/(platforms|imageformats|iconengines|styles)/[^/]+\.dll", name, re.I))
        if name not in allowed and not runtime and not plugins:
            raise ValueError(f"Unexpected file in {profile} bundle: {name}")
        if platform == "linux" and (name in spec["required_binaries"] or name.startswith("bin/geocoin-")) and not mode & 0o111:
            raise ValueError(f"Release binary or launcher is not executable: {name}")
    if platform == "linux" and profile == "user" and not any(n.startswith("plugins/platforms/") for n in files):
        raise ValueError("Wallet bundle is missing Qt platform plugins")
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("stage", "check"))
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--profile", choices=("user", "admin", "sdk"), required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "stage":
            if args.build_dir is None:
                parser.error("stage requires --build-dir")
            stage(args.root, args.output, args.build_dir, args.profile, args.platform)
        else:
            count = check(args.output, args.profile, args.platform, args.root)
            print(f"Validated {args.platform}/{args.profile} bundle: {count} files")
    except (OSError, ValueError, KeyError, tarfile.TarError, zipfile.BadZipFile) as error:
        parser.exit(1, f"release bundle: {error}\n")


if __name__ == "__main__":
    main()
