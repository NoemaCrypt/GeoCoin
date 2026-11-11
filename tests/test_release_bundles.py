#!/usr/bin/env python3
"""Release payload separation, missing inputs, archive contents and packagers."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("release_bundle", ROOT / "scripts/release_bundle.py")
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)


class ReleaseBundles(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="geocoin-bundle-test-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.build = self.base / "build"
        self.build.mkdir()
        self.sdk = self.base / "sdk-build"
        self.sdk.mkdir()
        for name in ["geocoin-core", "geocoin-node", "geocoin-db", "geocoin-rpc-miner",
                     "geocoin-gpu-miner", "genesis", "mine_genesis", "geocoin-miner", "degree_node", "test_secret"]:
            for extension in ["", ".exe"]:
                path = self.build / (name + extension)
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)
        for name in ["geo.lib", "geo.dll", "libgeo.so", "libgeo.a"]:
            (self.sdk / name).write_text("SDK fixture")

    def staged(self, profile, platform):
        output = self.base / (platform + "-" + profile)
        bundle.stage(ROOT, output, self.build, profile, platform)
        if profile == "sdk":
            paths = ["lib/libgeo.so"] if platform == "linux" else ["lib/geo.lib", "bin/geo.dll"]
            for name in paths:
                target = output / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("SDK fixture")
        else:
            for name in bundle.manifest()["profiles"][profile]["required_binaries"]:
                if platform == "linux":
                    launcher = output / name
                    launcher.write_text("#!/bin/sh\nexit 0\n")
                    launcher.chmod(0o755)
            if platform == "windows":
                (output / "geocoin-node.cmd").write_text("@echo off\n")
        if profile == "user":
            (output / "bin/qt.conf").write_text("[Paths]\n")
            if platform == "linux":
                plugin = output / "plugins/platforms/libqoffscreen.so"
                plugin.parent.mkdir(parents=True)
                plugin.write_text("Qt fixture")
        return output

    def test_all_profiles_and_platforms(self):
        for platform in ["linux", "windows"]:
            for profile in ["user", "admin", "sdk"]:
                with self.subTest(platform=platform, profile=profile):
                    output = self.staged(profile, platform)
                    self.assertGreater(bundle.check(output, profile, platform), 0)
                    names = set(bundle.entries(output))
                    self.assertFalse(any("genesis" in name or "degree_node" in name or "testing" in name for name in names))
                    self.assertNotIn("WHATS_NEW.txt", names)
                    if profile == "user":
                        self.assertFalse(any(name.startswith(("include/", "deploy/")) for name in names))
                        self.assertNotIn("bin/geocoin-db" + (".exe" if platform == "windows" else ""), names)

    def test_missing_required_binary_fails_before_staging(self):
        (self.build / "geocoin-node").unlink()
        output = self.base / "missing"
        with self.assertRaisesRegex(ValueError, "Required release binary"):
            bundle.stage(ROOT, output, self.build, "user", "linux")
        self.assertFalse(output.exists())

    def test_optional_gpu_is_not_required(self):
        (self.build / "geocoin-gpu-miner").unlink()
        output = self.staged("user", "linux")
        bundle.check(output, "user", "linux")
        self.assertFalse((output / "bin/geocoin-gpu-miner").exists())

    def test_rejects_extra_files(self):
        output = self.staged("user", "linux")
        for name in ["WHATS_NEW.txt", "bin/genesis", "wallet.dat", "config/activation-manifest.testing.json",
                     "include/libgeo/libgeo.h", "lib/libgeo.so", "runtime-dependencies.txt", "bin/degree_node"]:
            with self.subTest(name=name):
                path = output / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("must not ship")
                with self.assertRaisesRegex(ValueError, "Unexpected file"):
                    bundle.check(output, "user", "linux")
                path.unlink()

    def test_sdk_requires_runtime_and_import_library(self):
        output = self.staged("sdk", "windows")
        (output / "bin/geo.dll").unlink()
        with self.assertRaisesRegex(ValueError, "bin/geo.dll"):
            bundle.check(output, "sdk", "windows")

    def test_non_wallet_profiles_reject_qt_runtime(self):
        for profile in ["admin", "sdk"]:
            output = self.staged(profile, "linux")
            target = output / "lib/libQt5Core.so.5"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("unneeded Qt runtime")
            with self.assertRaisesRegex(ValueError, "Unexpected file"):
                bundle.check(output, profile, "linux")

    def test_linux_requires_executable_launchers(self):
        output = self.staged("admin", "linux")
        (output / "geocoin-node").chmod(0o644)
        with self.assertRaisesRegex(ValueError, "not executable"):
            bundle.check(output, "admin", "linux")

    def test_checks_actual_archive_payload(self):
        for platform in ["linux", "windows"]:
            output = self.staged("user", platform)
            archive = self.base / (platform + (".tar.gz" if platform == "linux" else ".zip"))
            if platform == "linux":
                with tarfile.open(archive, "w:gz") as target:
                    target.add(output, arcname="GeoCoin")
            else:
                with zipfile.ZipFile(archive, "w") as target:
                    for path in output.rglob("*"):
                        if path.is_file():
                            target.write(path, path.relative_to(output))
            self.assertEqual(bundle.check(archive, "user", platform), bundle.check(output, "user", platform))

    def test_rejects_archive_path_escape(self):
        for name in ["../wallet.dat", "/wallet.dat", "C:/wallet.dat", "bin\\wallet.dat"]:
            archive = self.base / "bad.zip"
            with zipfile.ZipFile(archive, "w") as target:
                target.writestr(name, "bad")
            with self.assertRaises(ValueError):
                bundle.entries(archive)

    def test_rejects_archive_links(self):
        archive = self.base / "bad.tar.gz"
        with tarfile.open(archive, "w:gz") as target:
            item = tarfile.TarInfo("GeoCoin/bin/geocoin-node")
            item.type = tarfile.SYMTYPE
            item.linkname = "/etc/passwd"
            target.addfile(item)
        with self.assertRaisesRegex(ValueError, "links"):
            bundle.entries(archive)

    @unittest.skipUnless(os.name != "nt" and shutil.which("patchelf"), "Linux packager needs patchelf")
    def test_linux_packager_profiles(self):
        plugins = self.base / "qt/platforms"
        plugins.mkdir(parents=True)
        (plugins / "libqoffscreen.so").write_text("Qt fixture")
        for profile in ["user", "admin", "sdk"]:
            output = self.base / ("packed-" + profile)
            archive = self.base / (profile + ".tar.gz")
            env = {**os.environ, "BUNDLE_PROFILE": profile, "GEO_BUILD_DIR": str(self.build),
                   "LIBGEO_BUILD_DIR": str(self.sdk), "OUT_DIR": str(output), "ARCHIVE_PATH": str(archive),
                   "REPORT_PATH": str(self.base / (profile + "-report.txt")),
                   "GEOCOIN_QT_PLUGIN_DIR": str(plugins.parent)}
            result = subprocess.run(["bash", str(ROOT / "scripts/package_portable_linux.sh")],
                                    env=env, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            bundle.check(archive, profile, "linux")

    @unittest.skipUnless(shutil.which("pwsh") or shutil.which("powershell"), "PowerShell not installed")
    def test_windows_packager_profiles(self):
        powershell = shutil.which("pwsh") or shutil.which("powershell")
        for directory in [self.build, self.sdk]:
            for name in ["Qt5Core.dll", "geo.dll", "libcrypto-3-x64.dll", "libssl-3-x64.dll"]:
                (directory / name).write_text("runtime fixture")
        for profile, dynamic in [(p, d) for p in ["user", "admin", "sdk"] for d in [False, True]]:
            output = self.base / ("windows-packed-" + profile + ("-dynamic" if dynamic else ""))
            result = subprocess.run([powershell, "-NoProfile", "-File", str(ROOT / "scripts/package_portable_windows.ps1"),
                                     "-RootPath", str(ROOT), "-GeoBuildDir", str(self.build),
                                     "-LibGeoBuildDir", str(self.sdk), "-OutputDir", str(output),
                                     "-BundleProfile", profile] + (["-Dynamic"] if dynamic else []),
                                    text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            bundle.check(output, profile, "windows")


if __name__ == "__main__":
    unittest.main()
