#!/usr/bin/env python3
"""Resolve a release tag to one commit, or verify it has not moved before upload."""
import argparse
import os
from pathlib import Path
import subprocess


def git(repo: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def resolve(repo: Path, tag: str, fallback: str, expected: str = "") -> str:
    if tag:
        ref = "refs/tags/" + tag
        git(repo, "check-ref-format", ref)
        if expected:
            # Fetch the current remote tag into FETCH_HEAD; never change the checkout.
            git(repo, "-c", "credential.helper=!gh auth git-credential",
                "fetch", "--no-tags", "origin", ref)
            ref = "FETCH_HEAD"
    else:
        ref = fallback
    sha = git(repo, "rev-parse", "--verify", "--end-of-options", ref + "^{commit}")
    if expected and sha != expected:
        raise ValueError("Release tag changed after source selection; rebuild before publishing")
    return sha


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--tag", default=os.environ.get("UPLOAD_TAG", ""))
    parser.add_argument("--fallback", default=os.environ.get("GITHUB_SHA", "HEAD"))
    parser.add_argument("--expected-sha", default="")
    args = parser.parse_args()
    sha = resolve(args.repo, args.tag, args.fallback, args.expected_sha)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"sha={sha}\n")
    print(sha)


if __name__ == "__main__":
    main()
