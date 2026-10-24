"""Verify the geodesy source pin in isolated local checkouts."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REVISION = "849284b9620b2ee4f0b9841b6822c67f9d0fa0c2"


class GeographicLibBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="geocoin-geodesy-pin-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "geographiclib"
        self.git("clone", "--quiet", "--shared", "--no-checkout",
                 str(ROOT / "geographiclib"), str(self.source), cwd=self.root)
        self.git("checkout", "--quiet", "--detach", REVISION)
        (self.root / "cmake").mkdir()
        shutil.copyfile(ROOT / "cmake/GeographicLib.cmake", self.root / "cmake/GeographicLib.cmake")
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            'project(GeodesyPinCheck LANGUAGES CXX)\n'
            'include(cmake/GeographicLib.cmake)\n'
            'get_target_property(kind geocoin_geographiclib TYPE)\n'
            'if(NOT kind STREQUAL "STATIC_LIBRARY")\n'
            '  message(FATAL_ERROR "Geodesy must be static")\n'
            'endif()\n', encoding="utf-8")

    def git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.source,
                              check=True, capture_output=True, text=True).stdout.strip()

    def configure(self):
        return subprocess.run(["cmake", "-S", str(self.root), "-B", str(self.root / "build"),
                               "-DBUILD_SHARED_LIBS=ON", "-DGEOGRAPHICLIB_PRECISION=3"],
                              capture_output=True, text=True)

    def test_exact_clean_source_static_binary64(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        config = (self.root / "build/geographiclib/include/GeographicLib/Config.h").read_text()
        self.assertIn("#define GEOGRAPHICLIB_PRECISION 2", config)
        self.assertIn("#define GEOGRAPHICLIB_SHARED_LIB 0", config)
        self.assertFalse((self.root / "build/CTestTestfile.cmake").exists())

    def test_different_revision_is_rejected(self):
        env = dict(os.environ, GIT_AUTHOR_NAME="Pin test", GIT_COMMITTER_NAME="Pin test",
                   GIT_AUTHOR_EMAIL="pin@example.invalid", GIT_COMMITTER_EMAIL="pin@example.invalid")
        subprocess.run(["git", "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "different revision"],
                       cwd=self.source, env=env, check=True, capture_output=True)
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GeographicLib must be at", result.stderr)

    def test_modified_source_is_rejected(self):
        with (self.source / "src/Geodesic.cpp").open("a") as output:
            output.write("\n// local modification\n")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checkout must be clean", result.stderr)

    def test_missing_submodule_is_rejected(self):
        shutil.rmtree(self.source)
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("git submodule update --init --recursive", " ".join(result.stderr.split()))

    def test_existing_build_rechecks_modified_source(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with (self.source / "src/Geodesic.cpp").open("a") as output:
            output.write("\n// source changed after configuration\n")
        result = subprocess.run(["cmake", "--build", str(self.root / "build"),
                                 "--target", "geocoin_geographiclib"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checkout must be clean", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
