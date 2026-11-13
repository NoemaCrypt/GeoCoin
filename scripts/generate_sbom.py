#!/usr/bin/env python3
from __future__ import annotations

import argparse
import configparser
import datetime as dt
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Any


def _vcpkg_manifest_components(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    baseline = str(manifest.get("builtin-baseline", ""))
    components: list[dict[str, Any]] = []
    for dependency in manifest.get("dependencies", []):
        if isinstance(dependency, str):
            name = dependency
            constraint = ""
            features: list[str] = []
        elif isinstance(dependency, dict) and isinstance(dependency.get("name"), str):
            name = dependency["name"]
            constraint = next(
                (
                    f"{key}{dependency[key]}"
                    for key in ("version", "version>=", "version<=", "version-string", "version-semver", "version-date")
                    if key in dependency
                ),
                "",
            )
            features = [str(feature) for feature in dependency.get("features", [])]
        else:
            continue

        properties = [{"name": "geocoin:package-manager", "value": "vcpkg"}]
        if baseline:
            properties.append({"name": "geocoin:vcpkg-baseline", "value": baseline})
        if constraint:
            properties.append({"name": "geocoin:manifest-constraint", "value": constraint})
        if features:
            properties.append({"name": "geocoin:vcpkg-features", "value": ",".join(features)})
        components.append({"type": "library", "name": name, "properties": properties})
    return components


def _vendored_components(root: Path) -> list[dict[str, Any]]:
    header = root / "include/nlohmann_json.hpp"
    if not header.is_file():
        return []
    content = header.read_bytes()
    versions = [re.search(rb"#define NLOHMANN_JSON_VERSION_" + part + rb"\s+(\d+)", content)
                for part in (b"MAJOR", b"MINOR", b"PATCH")]
    if not all(versions):
        raise ValueError("Cannot identify the vendored nlohmann JSON version")
    version = ".".join(match.group(1).decode("ascii") for match in versions)
    return [{"type": "library", "name": "nlohmann-json", "version": version,
             "licenses": [{"license": {"id": "MIT"}}],
             "hashes": [{"alg": "SHA-256", "content": hashlib.sha256(content).hexdigest()}],
             "properties": [{"name": "geocoin:source", "value": "vendored-header"}],
             "externalReferences": [{"type": "vcs", "url": "https://github.com/nlohmann/json"}]}]


def _submodule_components(root: Path) -> list[dict[str, Any]]:
    modules_path = root / ".gitmodules"
    if not modules_path.exists():
        return []

    parser = configparser.ConfigParser()
    parser.read(modules_path, encoding="utf-8")
    components: list[dict[str, Any]] = []
    for section in parser.sections():
        path = parser.get(section, "path", fallback="").strip()
        url = parser.get(section, "url", fallback="").strip()
        if not path:
            continue
        completed = subprocess.run(
            ["git", "-C", str(root / path), "rev-parse", "HEAD"],
            check=False,
            capture_output=True,
            text=True,
        )
        revision = completed.stdout.strip() if completed.returncode == 0 else ""
        component: dict[str, Any] = {
            "type": "library",
            "name": Path(path).name,
            "properties": [
                {"name": "geocoin:source", "value": "git-submodule"},
                {"name": "geocoin:path", "value": path},
            ],
        }
        if revision:
            component["version"] = revision
        if url:
            component["externalReferences"] = [{"type": "vcs", "url": url}]
        components.append(component)
    return components


def _resolved_vcpkg_components(
    search_roots: list[Path],
    platform: str,
) -> list[dict[str, Any]]:
    components: list[dict[str, Any]] = []
    seen_files: set[Path] = set()
    for search_root in search_roots:
        if not search_root.exists():
            continue
        for spdx_path in search_root.rglob("vcpkg.spdx.json"):
            resolved = spdx_path.resolve()
            if resolved in seen_files:
                continue
            seen_files.add(resolved)
            try:
                spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            packages = spdx.get("packages", [])
            if not isinstance(packages, list):
                continue
            # Every vcpkg SPDX contains a port recipe, a binary package and
            # source/resource packages. Only SPDXRef-port is the dependency
            # component/version; treating resources as packages creates many
            # unversioned false components such as archive and patch names.
            package = next(
                (
                    item
                    for item in packages
                    if isinstance(item, dict) and item.get("SPDXID") == "SPDXRef-port"
                ),
                None,
            )
            if package is None:
                continue
            name = package.get("name")
            version = package.get("versionInfo")
            if not isinstance(name, str) or not name or not isinstance(version, str) or not version:
                continue
            properties = [{"name": "geocoin:package-manager", "value": "vcpkg-resolved"}]
            if platform:
                properties.append({"name": "geocoin:platform", "value": platform})
            binary_package = next(
                (
                    item
                    for item in packages
                    if isinstance(item, dict) and item.get("SPDXID") == "SPDXRef-binary"
                ),
                None,
            )
            if binary_package is not None:
                binary_name = binary_package.get("name")
                if isinstance(binary_name, str) and binary_name.startswith(name + ":"):
                    properties.append(
                        {"name": "geocoin:vcpkg-triplet", "value": binary_name[len(name) + 1:]}
                    )
            component: dict[str, Any] = {
                "type": "library",
                "name": name,
                "version": version,
                "properties": properties,
            }
            components.append(component)
    return components


def _load_merged_sbom_components(paths: list[Path]) -> list[dict[str, Any]]:
    components: list[dict[str, Any]] = []
    for path in paths:
        try:
            sbom = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError(f"Could not read merged SBOM {path}: {error}") from error
        if sbom.get("bomFormat") != "CycloneDX" or not isinstance(sbom.get("components"), list):
            raise ValueError(f"Merged SBOM is not a CycloneDX document with components: {path}")
        for component in sbom["components"]:
            if isinstance(component, dict) and isinstance(component.get("name"), str):
                components.append(component)
    return components


def _property_values(component: dict[str, Any], name: str) -> set[str]:
    values: set[str] = set()
    for prop in component.get("properties", []):
        if isinstance(prop, dict) and prop.get("name") == name and isinstance(prop.get("value"), str):
            values.add(prop["value"])
    return values


def _merge_list_field(target: dict[str, Any], source: dict[str, Any], field: str) -> None:
    merged: list[Any] = []
    seen: set[str] = set()
    for item in [*target.get(field, []), *source.get(field, [])]:
        marker = json.dumps(item, sort_keys=True, separators=(",", ":"))
        if marker not in seen:
            seen.add(marker)
            merged.append(item)
    if merged:
        target[field] = merged


def _merge_component(target: dict[str, Any], source: dict[str, Any]) -> dict[str, Any]:
    merged = dict(target)
    for field in ("properties", "externalReferences", "licenses", "hashes"):
        _merge_list_field(merged, source, field)
    for key, value in source.items():
        if key not in merged or merged[key] in (None, "", [], {}):
            merged[key] = value
    return merged


def _deduplicate_components(components: list[dict[str, Any]]) -> list[dict[str, Any]]:
    manifest_by_name: dict[str, dict[str, Any]] = {}
    resolved_names: set[str] = set()
    for component in components:
        name = str(component.get("name", ""))
        package_managers = _property_values(component, "geocoin:package-manager")
        if name and not component.get("version") and "vcpkg" in package_managers:
            manifest_by_name[name] = _merge_component(manifest_by_name.get(name, {}), component)
        if name and component.get("version") and "vcpkg-resolved" in package_managers:
            resolved_names.add(name)

    normalized: list[dict[str, Any]] = []
    for component in components:
        name = str(component.get("name", ""))
        package_managers = _property_values(component, "geocoin:package-manager")
        if name in resolved_names and not component.get("version") and "vcpkg" in package_managers:
            continue
        if name in manifest_by_name and component.get("version") and "vcpkg-resolved" in package_managers:
            component = _merge_component(component, manifest_by_name[name])
        normalized.append(component)

    deduplicated: dict[tuple[str, str, str, str], dict[str, Any]] = {}
    for component in normalized:
        key = (
            str(component.get("type", "")),
            str(component.get("name", "")),
            str(component.get("version", "")),
            str(component.get("purl", "")),
        )
        deduplicated[key] = _merge_component(deduplicated.get(key, {}), component)
    return sorted(
        deduplicated.values(),
        key=lambda item: (
            str(item.get("name", "")),
            str(item.get("version", "")),
            str(item.get("purl", "")),
        ),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a CycloneDX SBOM for GeoCoin releases.")
    parser.add_argument(
        "output",
        nargs="?",
        default=None,
        help="Optional output path. Defaults to <repo>/out/release-sbom.json",
    )
    parser.add_argument(
        "--version",
        default="unknown",
        help="Optional release version string to store in the SBOM metadata",
    )
    parser.add_argument(
        "--platform",
        default="",
        help="Optional platform label attached to resolved dependency components",
    )
    parser.add_argument(
        "--resolved-root",
        action="append",
        default=[],
        help="Build/install tree to scan recursively for vcpkg.spdx.json; repeatable",
    )
    parser.add_argument(
        "--merge-sbom",
        action="append",
        default=[],
        help="CycloneDX SBOM whose components should be merged into the output; repeatable",
    )
    parser.add_argument(
        "--require-resolved-vcpkg",
        action="store_true",
        help="Fail unless at least one versioned vcpkg-resolved component is present",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    vcpkg_manifest = root / "vcpkg.json"
    output = Path(args.output).resolve() if args.output else (root / "out" / "release-sbom.json")

    components: list[dict[str, Any]] = []
    manifest: dict[str, Any] = {}

    if vcpkg_manifest.exists():
        manifest = json.loads(vcpkg_manifest.read_text(encoding="utf-8"))
        components.extend(_vcpkg_manifest_components(manifest))

    components.extend(_submodule_components(root))
    components.extend(_vendored_components(root))

    if args.resolved_root:
        resolved_roots = []
        for value in args.resolved_root:
            candidate = Path(value)
            if not candidate.is_absolute():
                candidate = root / candidate
            candidate = candidate.resolve()
            if not candidate.exists():
                parser.error(f"Resolved dependency root does not exist: {candidate}")
            resolved_roots.append(candidate)
    else:
        resolved_roots = [root / "vcpkg_installed", root / "out" / "build"]
    components.extend(_resolved_vcpkg_components(resolved_roots, args.platform.strip()))

    merge_paths: list[Path] = []
    for value in args.merge_sbom:
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = root / candidate
        candidate = candidate.resolve()
        if not candidate.is_file():
            parser.error(f"Merged SBOM does not exist: {candidate}")
        merge_paths.append(candidate)
    try:
        components.extend(_load_merged_sbom_components(merge_paths))
    except ValueError as error:
        parser.error(str(error))

    components = _deduplicate_components(components)
    if args.require_resolved_vcpkg and not any(
        component.get("version") and
        "vcpkg-resolved" in _property_values(component, "geocoin:package-manager")
        for component in components
    ):
        parser.error("No versioned vcpkg-resolved components were found")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(
            {
                "bomFormat": "CycloneDX",
                "specVersion": "1.5",
                "version": 1,
                "metadata": {
                    "timestamp": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
                    "tools": [{"vendor": "GeoCoin", "name": "scripts/generate_sbom.py"}],
                    "component": {
                        "name": "GeoCoin",
                        "type": "application",
                        "version": args.version,
                        **(
                            {"properties": [{"name": "geocoin:platform", "value": args.platform.strip()}]}
                            if args.platform.strip()
                            else {}
                        ),
                    },
                },
                "components": components,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
