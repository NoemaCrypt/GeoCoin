#!/usr/bin/env python3
"""Check local Markdown links in README and authored docs, including Git eligibility."""
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit


def check(root: Path) -> list[str]:
    errors = []
    for source in [root / "README.md", *sorted((root / "docs").rglob("*.md"))]:
        if "generated" in source.relative_to(root).parts:
            continue
        content = re.sub(r"```.*?```|~~~.*?~~~", "", source.read_text(encoding="utf-8"), flags=re.S)
        for match in re.finditer(r"\]\((?:<([^>]+)>|([^\s)]+))", content):
            link = urlsplit(match[1] or match[2])
            if link.scheme or link.netloc or not link.path:
                continue
            target = (source.parent / unquote(link.path)).resolve()
            label = f"{source.relative_to(root)}: {link.path}"
            if not target.is_relative_to(root):
                errors.append(f"{label}: local link leaves the repository")
            elif not target.exists():
                errors.append(f"{label}: target does not exist")
            elif subprocess.run(["git", "-C", str(root), "check-ignore", "-q", "--", str(target)]).returncode == 0:
                errors.append(f"{label}: target is excluded from Git")
    return errors


if __name__ == "__main__":
    problems = check(Path(__file__).resolve().parents[1])
    if problems:
        print("\n".join(problems), file=sys.stderr)
        raise SystemExit(1)
    print("Authored documentation links and Git inclusion passed")
