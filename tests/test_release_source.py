#!/usr/bin/env python3
"""Release selection regressions using isolated local Git repositories."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release_source", ROOT / "scripts/resolve_release_source.py")
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


class ReleaseSourceTests(unittest.TestCase):
    def test_tag_selection_and_movement(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "origin"
            repo.mkdir()
            def git(*args):
                return source.git(repo, *args)
            git("init", "-q")
            git("config", "user.name", "Release test")
            git("config", "user.email", "release-test@example.invalid")
            git("-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "release version")
            tagged_sha = git("rev-parse", "HEAD")
            git("tag", "-a", "v-test", "-m", "Annotated release")
            git("-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "newer workflow branch")
            head_sha = git("rev-parse", "HEAD")
            self.assertNotEqual(tagged_sha, head_sha)
            self.assertEqual(source.resolve(repo, "v-test", "HEAD"), tagged_sha)
            self.assertEqual(source.resolve(repo, "", head_sha), head_sha)
            for tag in ["missing", "../bad", "v-test^{commit}"]:
                with self.assertRaises(subprocess.CalledProcessError):
                    source.resolve(repo, tag, "HEAD")
            checkout = Path(directory) / "checkout"
            subprocess.run(["git", "clone", "-q", str(repo), str(checkout)], check=True)
            self.assertEqual(source.resolve(checkout, "v-test", "HEAD", tagged_sha), tagged_sha)
            git("tag", "-f", "v-test", head_sha)
            with self.assertRaises(ValueError):
                source.resolve(checkout, "v-test", "HEAD", tagged_sha)

    def test_manifest_records_tested_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            bundle = base / "portable.zip"
            bundle.write_bytes(b"example release artifact")
            output = base / "manifest.json"
            sha = "0123456789abcdef" * 2 + "01234567"
            subprocess.run([sys.executable, str(ROOT / "scripts/generate_release_manifest.py"), str(output),
                            str(bundle), "--base-dir", str(base), "--source-commit", sha,
                            "--release-tag", "v-test"], check=True)
            manifest = json.loads(output.read_text())
            self.assertEqual(manifest["source_commit"], sha)
            self.assertEqual(manifest["release_tag"], "v-test")
            self.assertEqual(manifest["artifacts"][0]["path"], bundle.name)
            self.assertEqual(manifest["artifacts"][0]["sha256"], hashlib.sha256(bundle.read_bytes()).hexdigest())

    def test_release_workflow_publishes_only_a_complete_draft(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text()
        self.assertIn("push:\n    tags:", workflow)
        self.assertNotIn("types: [published]", workflow)
        self.assertIn("github.workflow_sha", workflow)
        self.assertIn(".release-tooling/generate_release_manifest.py", workflow)
        self.assertIn(".release-tooling/generate_checksums.py", workflow)
        self.assertIn(".release-tooling/generate_sbom.py", workflow)
        self.assertIn("--expected-sha \"$SOURCE_SHA\"", workflow)
        self.assertNotIn("--clobber", workflow)

        create = workflow.index("gh release create")
        upload = workflow.index("gh release upload")
        verify = workflow.index("Draft release asset set does not match")
        publish = workflow.index('gh release edit "$UPLOAD_TAG" --draft=false')
        self.assertLess(create, upload)
        self.assertLess(upload, verify)
        self.assertLess(verify, publish)
        self.assertIn("--draft", workflow[create:upload])
        self.assertIn("isDraft", workflow)
        self.assertIn("refusing to mutate it", workflow)
        self.assertIn("release-manifest.json", workflow[upload:verify])
        self.assertIn("SHA256SUMS.txt", workflow[upload:verify])


if __name__ == "__main__":
    unittest.main()
