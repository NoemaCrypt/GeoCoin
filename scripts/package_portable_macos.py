#!/usr/bin/env python3
"""Build and audit relocatable GeoCoin releases for Apple Silicon."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

from release_bundle import ROOT, entries, manifest, notices, stage

MINIMUM_SYSTEM = "14.0"
SYSTEM_PATHS = ("/usr/lib/", "/System/Library/")
LOAD_DYLIB_COMMANDS = {
    "LC_LOAD_DYLIB",
    "LC_LOAD_WEAK_DYLIB",
    "LC_LAZY_LOAD_DYLIB",
    "LC_REEXPORT_DYLIB",
    "LC_LOAD_UPWARD_DYLIB",
}
DEFAULT_DEPENDENCIES = ROOT / "out/macos-deps/installed/arm64-osx-geocoin"
PACKAGING_PYTHON = ROOT / "out/macos-deps/packaging/bin/python"
APP_NAME = "GeoCoin Core.app"


def command(*args, capture=False, quiet=False):
    result = subprocess.run(
        args, check=not quiet, text=True,
        stdout=subprocess.PIPE if capture or quiet else None,
        stderr=subprocess.STDOUT if quiet else None,
    )
    if result.returncode != 0:
        raise ValueError(f"{args[0]} failed for {args[-1]}:\n{result.stdout}")
    return result.stdout or ""


@dataclass(frozen=True)
class Signing:
    identity: str = "-"
    keychain: Path | None = None
    notary_profile: str | None = None

    def sign(self, target, *, disk_image=False):
        flags = ["--force", "--sign", self.identity]
        if self.keychain is not None:
            flags.extend(("--keychain", self.keychain))
        if self.identity != "-":
            flags.append("--timestamp")
            if disk_image:
                flags.extend(("--identifier", "org.noemacrypt.geocoin-disk-image"))
            else:
                flags.extend(("--options", "runtime"))
        command("codesign", *flags, target, quiet=True)

    def notarize(self, upload, staple):
        if self.notary_profile is None:
            return
        credentials = ["--keychain-profile", self.notary_profile]
        if self.keychain is not None:
            credentials.extend(("--keychain", self.keychain))
        response = json.loads(command(
            "xcrun", "notarytool", "submit", upload, *credentials,
            "--wait", "--timeout", "45m", "--output-format", "json", capture=True,
        ))
        if response.get("status") != "Accepted":
            submission = response.get("id")
            if submission:
                command("xcrun", "notarytool", "log", submission, *credentials)
            raise ValueError(f"Apple notarization failed for {upload.name}: {response.get('status')}")
        command("xcrun", "stapler", "staple", staple)
        command("xcrun", "stapler", "validate", staple)


def load_commands(binary):
    contents = command("otool", "-arch", "arm64", "-l", binary, capture=True)
    for entry in contents.split("Load command ")[1:]:
        fields = {}
        for line in entry.splitlines()[1:]:
            key, _, value = line.strip().partition(" ")
            fields[key] = value.split(" (offset ", 1)[0]
        yield fields


def os_version(version):
    values = tuple(int(field) for field in version.split("."))
    return values + (0,) * (3 - len(values))


def check_target(image, commands):
    if command("lipo", "-archs", image, capture=True).strip() != "arm64":
        raise ValueError(f"Expected an arm64-only Mach-O image at {image}")
    for data in commands:
        if data.get("cmd") == "LC_BUILD_VERSION":
            platform_name = data.get("platform", "").lower()
            minimum = data.get("minos")
            if platform_name in ("macos", "1") and minimum:
                if os_version(minimum) <= os_version(MINIMUM_SYSTEM):
                    return minimum
                raise ValueError(
                    f"{image}: deployment target {minimum} exceeds macOS {MINIMUM_SYSTEM}"
                )
    raise ValueError(f"{image}: no macOS deployment target")


def anchor_path(reference, loader):
    if reference.startswith("@loader_path/"):
        return loader.parent / reference.removeprefix("@loader_path/")
    if reference.startswith("@executable_path/"):
        return loader.parent / reference.removeprefix("@executable_path/")
    return Path(reference)


def resolve_dependency(reference, source, commands, dependencies):
    if reference.startswith("@rpath/"):
        relative = reference.removeprefix("@rpath/")
        candidates = [
            anchor_path(data["path"], source) / relative
            for data in commands
            if data.get("cmd") == "LC_RPATH"
        ]
        candidates.extend((dependencies / "lib" / relative, source.parent / relative))
    else:
        candidates = [anchor_path(reference, source)]
    for candidate in candidates:
        if candidate.is_file():
            candidate = candidate.resolve()
            if candidate.is_relative_to(dependencies):
                return candidate
            raise ValueError(f"{source}: dependency is outside the pinned runtime: {candidate}")
    raise ValueError(f"{source}: cannot resolve {reference}")


def install_runtime(initial_images, libraries, dependencies, signing=Signing()):
    libraries.mkdir(parents=True, exist_ok=True)
    pending = dict(initial_images)
    sources = {}
    targets = {}
    while pending:
        target, source = pending.popitem()
        sources[target] = source
        commands = tuple(load_commands(source))
        check_target(source, commands)
        changes = []
        for data in commands:
            kind = data.get("cmd")
            if kind == "LC_RPATH":
                changes.extend(("-delete_rpath", data["path"]))
            elif kind == "LC_ID_DYLIB":
                changes.extend(("-id", f"@rpath/{target.name}"))
            elif kind in LOAD_DYLIB_COMMANDS:
                reference = data["name"]
                if reference.startswith(SYSTEM_PATHS):
                    continue
                dependency = resolve_dependency(reference, source, commands, dependencies)
                name = dependency.name if dependency.suffix == ".dylib" else f"{dependency.name}.dylib"
                destination = libraries / name
                previous = sources.get(destination) or pending.get(destination)
                if previous is None:
                    pending[destination] = dependency
                elif dependency != previous:
                    raise ValueError(f"Conflicting Mach-O files named {name}")
                relative = os.path.relpath(destination, target.parent)
                changes.extend(("-change", reference, f"@loader_path/{relative}"))
        targets[target] = changes

    for target, source in sources.items():
        if target.resolve() != source.resolve():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        if targets[target]:
            command("install_name_tool", *targets[target], target, quiet=True)
        signing.sign(target)

    return sources.keys()


def audit_runtime(root, images, report):
    lines = []
    expected = set(images)
    for image in sorted(images):
        commands = tuple(load_commands(image))
        minimum = check_target(image, commands)
        command("codesign", "--verify", "--strict", image)
        lines.append(f"{image.relative_to(root)} (arm64, macOS {minimum})")
        for data in commands:
            kind = data.get("cmd")
            if kind == "LC_RPATH":
                raise ValueError(f"{image}: unexpected runtime search path {data['path']}")
            if kind not in LOAD_DYLIB_COMMANDS:
                continue
            reference = data["name"]
            if not reference.startswith(SYSTEM_PATHS):
                if not reference.startswith("@loader_path/"):
                    raise ValueError(f"{image}: unexpected dependency path {reference}")
                resolved = anchor_path(reference, image).resolve()
                if not resolved.is_relative_to(root) or resolved not in expected:
                    raise ValueError(f"{image}: unstaged dependency {reference}")
            lines.append(f"  {reference}")
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")


def plugins_directory(dependencies):
    plugin_path = os.environ.get("GEOCOIN_QT_PLUGIN_DIR")
    if plugin_path:
        candidates = [Path(plugin_path)]
    else:
        candidates = [
            dependencies / "plugins",
            dependencies / "lib/qt5/plugins",
            dependencies / "tools/qt5/plugins",
            dependencies / "share/qt5/plugins",
        ]
    for path in candidates:
        if (path / "platforms/libqcocoa.dylib").is_file():
            return path
    raise ValueError(f"Cannot find the Qt 5 Cocoa plugin under {dependencies}")


def plugin_images(dependencies, plugin_target):
    plugin_groups = {"platforms", "imageformats", "iconengines", "styles"}
    platform_names = {"libqcocoa.dylib", "libqminimal.dylib", "libqoffscreen.dylib"}
    images = {}
    for path in plugins_directory(dependencies).glob("*/*.dylib"):
        group = path.parent.name
        if group not in plugin_groups or group == "platforms" and path.name not in platform_names:
            continue
        images[plugin_target / group / path.name] = path
    return images


def write_qt_config(destination, plugins):
    destination.write_text(f"[Paths]\nPrefix=.\nPlugins={plugins}\n", encoding="utf-8")


def iconset(resources, scratch):
    icons = scratch / "GeoCoin.iconset"
    icons.mkdir()
    source = ROOT / "src/qt/res/icons/geocoin.png"
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            pixels = str(size * scale)
            suffix = "@2x" if scale == 2 else ""
            image = icons / f"icon_{size}x{size}{suffix}.png"
            command("sips", "-z", pixels, pixels, source, "--out", image, capture=True)
    command("iconutil", "-c", "icns", "-o", resources / "GeoCoin.icns", icons)


def build_app(app, build, dependencies, user_bundle, scratch, signing):
    contents = app / "Contents"
    executable = contents / "MacOS/geocoin-core"
    resources = contents / "Resources"
    resources.mkdir(parents=True)
    version = re.search(
        r"project\(GeoCoin VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text()
    ).group(1)
    metadata = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "GeoCoin Core",
        "CFBundleExecutable": executable.name,
        "CFBundleIconFile": "GeoCoin.icns",
        "CFBundleIdentifier": "org.noemacrypt.geocoin-core",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "GeoCoin Core",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSMinimumSystemVersion": MINIMUM_SYSTEM,
        "NSHighResolutionCapable": True,
    }
    with (contents / "Info.plist").open("wb") as file:
        plistlib.dump(metadata, file)
    iconset(resources, scratch)
    write_qt_config(resources / "qt.conf", "../PlugIns")
    shutil.copytree(user_bundle / "licenses", resources / "licenses")
    shutil.copytree(user_bundle / "config", resources / "config")
    images = {
        executable: build / executable.name,
        contents / "Helpers/geocoin-gpu-miner": build / "geocoin-gpu-miner",
    }
    images.update(plugin_images(dependencies, contents / "PlugIns"))
    packaged = install_runtime(images, contents / "Frameworks", dependencies, signing)
    signing.sign(app)
    command("codesign", "--verify", "--deep", "--strict", app)
    audit_runtime(app, packaged, scratch / "app-dependencies.txt")
    if signing.notary_profile is not None:
        app_archive = scratch / "GeoCoin-Core-notary.zip"
        command("ditto", "-c", "-k", "--keepParent", app, app_archive)
        signing.notarize(app_archive, app)
    return executable


def smoke_gui(executable, scratch):
    test_home = scratch / "home"
    test_home.mkdir(exist_ok=True)
    environment = {
        **os.environ,
        "HOME": str(test_home),
        "CFFIXED_USER_HOME": str(test_home),
        "QT_DEBUG_PLUGINS": "1",
        "GEOCOIN_SMOKE_TRACE_FILE": str(scratch / "smoke-trace.txt"),
    }
    for key in tuple(environment):
        if key.startswith(("DYLD_", "QT_PLUGIN_", "QT_QPA_", "GEOCOIN_QT_WALLET_")) or key in (
            "QT_MESSAGE_PATTERN", "QT_LOGGING_RULES",
        ):
            environment.pop(key)
    result = subprocess.run(
        [
            executable,
            "-platform", "cocoa",
            "--smoke-test",
            "-no-auto-connect",
            "--data-dir", scratch / "state",
        ],
        cwd=scratch,
        env=environment,
        check=False,
        text=True,
        timeout=90,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode != 0:
        raise ValueError(f"Packaged Cocoa smoke failed ({result.returncode}):\n{result.stdout}")
    plugin = re.search(r'^loaded library "([^"\n]+/libqcocoa\.dylib)"$', result.stdout, re.MULTILINE)
    if plugin is None:
        raise ValueError(f"Packaged Cocoa smoke did not load a Cocoa plugin: {executable}")
    if not Path(plugin[1]).resolve().is_relative_to(executable.parents[1]):
        raise ValueError(f"Packaged Cocoa smoke resolved a host plugin: {executable}")
    print(f"Packaged Cocoa smoke passed: {executable.name}")


def check_disk_image(disk_image, scratch, smoke):
    mount = scratch / "mounted-dmg"
    mount.mkdir()
    command("hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, disk_image, quiet=True)
    try:
        app = mount / APP_NAME
        command("codesign", "--verify", "--deep", "--strict", app)
        if smoke:
            smoke_gui(app / "Contents/MacOS/geocoin-core", scratch)
    finally:
        command("hdiutil", "detach", mount, quiet=True)


def move_output(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.is_dir():
        shutil.rmtree(destination)
    shutil.move(source, destination)


def release_metadata(args, archive, disk_image):
    payload = [archive]
    if disk_image is not None:
        payload.append(disk_image)
    base = archive.parent
    source = os.environ.get("RELEASE_SOURCE_COMMIT")
    tag = os.environ.get("RELEASE_TAG")
    metadata = []
    if source:
        metadata.extend(("--source-commit", source))
    if tag:
        metadata.extend(("--release-tag", tag))
    command(
        sys.executable, ROOT / "scripts/generate_release_manifest.py", args.manifest,
        "--base-dir", base, *metadata, *payload,
    )
    command(
        sys.executable, ROOT / "scripts/generate_checksums.py", args.checksums,
        "--base-dir", base, *payload, args.manifest,
    )


def check_bundle(source, profile):
    data = manifest()
    spec = data["profiles"][profile]
    files = entries(source)
    required = set(data["common_files"].values()) | set(spec["files"].values())
    binaries = set(spec["required_binaries"])
    if profile == "user":
        binaries.add("geocoin-gpu-miner")
        required.add("bin/qt.conf")
    elif profile == "sdk":
        required.add("lib/libgeo.dylib")
    required.update(binaries)
    required.update(f"bin/{name}" for name in binaries)
    missing = required - files.keys()
    if missing:
        raise ValueError("Required bundle files missing: " + ", ".join(sorted(missing)))
    for name, mode in files.items():
        if name in binaries or name.startswith("bin/geocoin-"):
            if not mode & 0o111:
                raise ValueError(f"Release binary or launcher is not executable: {name}")
        if name in required:
            continue
        if re.fullmatch(r"licenses/dependencies/[A-Za-z0-9_.+-]+\.txt", name):
            continue
        if profile == "user" and re.fullmatch(
            r"bin/plugins/(platforms|imageformats|iconengines|styles)/[^/]+\.dylib", name
        ):
            continue
        if re.fullmatch(r"lib/[^/]+\.dylib", name) and not name.startswith("lib/libgeo."):
            if profile == "user" or not name.startswith(("lib/libQt", "lib/Qt")):
                continue
        raise ValueError(f"Unexpected file in {profile} macOS bundle: {name}")
    if profile == "user" and "bin/plugins/platforms/libqcocoa.dylib" not in files:
        raise ValueError("Wallet bundle is missing the Cocoa platform plugin")
    return len(files)


def package(args):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("GeoCoin macOS releases must be packaged on native Apple Silicon")
    if not args.dependencies.is_dir():
        raise ValueError("Run scripts/bootstrap-macos.sh --profile release first")
    if args.profile == "user" and not PACKAGING_PYTHON.is_file():
        raise ValueError("Run scripts/bootstrap-macos.sh --profile release to install DMG layout tooling")
    signing = Signing(args.sign_identity, args.sign_keychain, args.notary_profile)
    if signing.notary_profile is not None and signing.identity == "-":
        raise ValueError("Notarization requires a Developer ID Application signing identity")
    if ROOT.is_relative_to(args.output) or (
        args.output.is_relative_to(ROOT) and not args.output.is_relative_to(ROOT / "out")
    ) or any(
        path.is_relative_to(args.output) for path in (args.build_dir, args.sdk, args.dependencies)
    ):
        raise ValueError("Bundle output directory overlaps the source, build or runtime inputs")
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=".geocoin-package-", dir=args.output.parent) as scratch:
        scratch = Path(scratch)
        bundle = scratch / args.output.name
        stage(ROOT, bundle, args.build_dir, args.profile, "macos")
        shutil.copyfile(ROOT / "docs/macos" / f"release-{args.profile}.txt", bundle / "README.txt")
        bin_dir = bundle / "bin"
        lib_dir = bundle / "lib"
        lib_dir.mkdir()
        images = {}
        for binary in bin_dir.glob("geocoin-*"):
            images[binary] = args.build_dir / binary.name
            launcher = bundle / binary.name
            launcher.write_text(
                '#!/bin/sh\nset -eu\n'
                'root="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\n'
                f'exec "$root/bin/{binary.name}" "$@"\n',
                encoding="utf-8",
            )
            launcher.chmod(0o755)

        if args.profile == "user":
            if bin_dir / "geocoin-gpu-miner" not in images:
                raise ValueError("macOS user bundle requires the Metal/OpenCL miner")
            write_qt_config(bin_dir / "qt.conf", "plugins")
            images.update(plugin_images(args.dependencies, bin_dir / "plugins"))
        elif args.profile == "sdk":
            images[lib_dir / "libgeo.dylib"] = args.sdk / "libgeo.dylib"

        packaged = install_runtime(images, lib_dir, args.dependencies, signing)
        audit_runtime(bundle, packaged, scratch / "dependencies.txt")
        notices(bundle, "macos", [], args.dependencies)
        count = check_bundle(bundle, args.profile)
        print(f"Validated macos/{args.profile} bundle: {count} files")

        archive = scratch / args.archive.name
        command("tar", "--no-mac-metadata", "--no-xattrs", "-czf", archive, "-C", scratch, bundle.name)
        check_bundle(archive, args.profile)

        disk_image = None
        if args.profile == "user":
            app = scratch / "dmg" / APP_NAME
            executable = build_app(app, args.build_dir, args.dependencies, bundle, scratch, signing)
            if args.smoke:
                smoke_gui(bin_dir / "geocoin-core", scratch)
                smoke_gui(executable, scratch)
            disk_image = scratch / args.dmg.name
            command(PACKAGING_PYTHON, ROOT / "scripts/build_macos_dmg.py", app, disk_image)
            if signing.identity != "-":
                signing.sign(disk_image, disk_image=True)
                command("codesign", "--verify", "--strict", disk_image)
                signing.notarize(disk_image, disk_image)
            command("hdiutil", "verify", disk_image)
            check_disk_image(disk_image, scratch, args.smoke)
            move_output(app, args.app)
            move_output(disk_image, args.dmg)
            move_output(scratch / "app-dependencies.txt", args.report.with_name("macos-app-dependencies.txt"))
        move_output(bundle, args.output)
        move_output(archive, args.archive)
        move_output(scratch / "dependencies.txt", args.report)
        release_metadata(args, args.archive, args.dmg if disk_image else None)

    print(f"Portable archive: {args.archive}")
    if disk_image:
        print(f"Finder app: {args.app}\nDisk image: {args.dmg}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("user", "admin", "sdk"), default=os.getenv("BUNDLE_PROFILE", "user"))
    parser.add_argument("--build-dir", type=Path, default=os.getenv("GEO_BUILD_DIR", ROOT / "out/build/macos-arm64-release"))
    parser.add_argument("--sdk", type=Path, default=os.getenv("LIBGEO_BUILD_DIR", ROOT / "libgeo/out/build/macos-arm64-release"))
    parser.add_argument("--dependencies", type=Path, default=DEFAULT_DEPENDENCIES)
    parser.add_argument("--smoke", action="store_true", help="Launch the staged binary and app with Cocoa and isolated node data")
    parser.add_argument("--sign-identity", default=os.getenv("GEOCOIN_MACOS_SIGN_IDENTITY") or "-",
                        help="Developer ID Application identity. Local bundles default to ad-hoc signing")
    parser.add_argument("--sign-keychain", type=Path, default=os.getenv("GEOCOIN_MACOS_SIGN_KEYCHAIN") or None,
                        help="Keychain containing the signing identity and optional notarization profile")
    parser.add_argument("--notary-profile", default=os.getenv("GEOCOIN_MACOS_NOTARY_PROFILE") or None,
                        help="Submit Developer ID-signed app and DMG using an existing notarytool profile")
    args = parser.parse_args()
    for option in ("build_dir", "sdk", "dependencies"):
        setattr(args, option, getattr(args, option).resolve())
    profile_suffix = "" if args.profile == "user" else f"-{args.profile}"
    base = ROOT / "out/portable" / f"GeoCoin-macos-arm64{profile_suffix}"
    for key, env, default in (
        ("output", "OUT_DIR", base),
        ("archive", "ARCHIVE_PATH", base.with_name(base.name + "-portable.tar.gz")),
        ("manifest", "MANIFEST_PATH", base.with_name(base.name + "-manifest.json")),
        ("checksums", "CHECKSUM_PATH", base.with_name(base.name + "-SHA256SUMS.txt")),
        ("report", "REPORT_PATH", ROOT / "out/packaging" / f"macos-{args.profile}-runtime-dependencies.txt"),
        ("app", "APP_PATH", base.parent / APP_NAME),
        ("dmg", "DMG_PATH", base.with_suffix(".dmg")),
    ):
        setattr(args, key, Path(os.getenv(env, default)).resolve())
    try:
        package(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"macOS package: {error}\n")


if __name__ == "__main__":
    main()
