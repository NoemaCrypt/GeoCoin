"""Exercise an offline backup and restore of four real, consensus-populated DBs."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


fixture, tool = sys.argv[1:3]
selected = sys.argv[3] if len(sys.argv) == 4 else None
damage_cases = ("merkle", "witness", "active-index", "membership-height", "query-height",
                "query-syntax", "query-value", "orphan-reverse", "payload-hash", "payload-data",
                "duplicate-column", "misplaced-column", "misplaced-state", "unknown-record",
                "missing-foundation", "missing-interest-pool", "malformed-deposit", "malformed-card",
                "malformed-undo", "orphan-nft", "orphan-witness")
card_cases = tuple("card-" + case for case in (
    "orphan-program", "issuer-mismatch", "invalid-issuer", "invalid-status",
    "zero-valid-from", "equal-validity", "reversed-validity", "future-program",
    "early-issue", "reversed-heights", "future-update", "null-program-id",
    "null-pubkey", "null-metadata", "empty-vendor", "invalid-os", "zero-applet-version",
    "null-package", "null-scope", "zero-quorum", "excess-quorum", "duplicate-member",
    "unsorted-members", "invalid-member", "duplicate-pubkey"
))
damage_cases += card_cases
sidechain_cases = tuple("sidechain-" + case for case in (
    "missing-deposit", "owner-mismatch", "invalid-owner", "zero-unlock", "future-checkpoint",
    "missing-checkpoint-height", "missing-sidechain-height", "large-sidechain-height", "null-checkpoint",
    "zero-threshold", "excess-threshold", "committee-count", "committee-id", "duplicate-member",
    "unsorted-members", "invalid-member", "low-multiplier", "reversed-multiplier", "large-multiplier",
    "large-fee", "financial-policy", "financial-version", "empty-symbol", "bad-symbol",
    "future-decommission", "bad-namespace", "duplicate-namespace", "null-id", "missing-registry"
))
sidechain_valid = tuple("sidechain-" + case for case in ("valid", "accounting", "unregistered", "slashed", "decommissioned"))
damage_cases += sidechain_cases
pruning_cases = ("prune-bodies", "prune-indexes", "prune-witness")
assert selected is None or selected in damage_cases + pruning_cases + ("card-valid",) + sidechain_valid, "Unknown snapshot test case"


def run(*args, succeeds=True):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert (result.returncode == 0) == succeeds, result.stdout
    return result.stdout


def inventory(root):
    result = {}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative == "snapshot.json" or path.name in ("LOCK", "LOG") or path.name.startswith("LOG.old."):
            continue
        data = path.read_bytes()
        result[relative] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return result


def round_trip(root, source):
    backup, restored = root / "backup", root / "restored"
    run(tool, "check", str(source))
    run(tool, "backup", str(source), str(backup))
    run(tool, "verify", str(backup))
    run(tool, "restore", str(backup), str(restored))
    run(tool, "verify", str(restored))
    manifest = json.loads((backup / "snapshot.json").read_text())
    assert manifest["state"]["height"] == 3 and manifest["state"]["utxos"] > 0
    assert json.loads((restored / "snapshot.json").read_text()) == manifest
    run(fixture, "--check-snapshot-fixture", str(restored))
    run(tool, "check", str(restored))
    return backup, restored


def reject_damaged(root, source, case):
    expected_state = json.loads(run(tool, "check", str(source)))
    damaged = root / (case + "-source")
    shutil.copytree(source, damaged)
    run(fixture, "--alter-snapshot-fixture", str(damaged), case)
    run(tool, "check", str(damaged), succeeds=False)
    destination = root / (case + "-backup")
    run(tool, "backup", str(damaged), str(destination), succeeds=False)
    assert not destination.exists()
    # Checksums match these bytes. Verification must reject their inconsistent
    # records, independently of the manifest's transport integrity checks.
    (damaged / "snapshot.json").write_text(json.dumps({
        "version": 1, "state": expected_state, "files": inventory(damaged)
    }))
    run(tool, "verify", str(damaged), succeeds=False)
    destination = root / (case + "-restore")
    run(tool, "restore", str(damaged), str(destination), succeeds=False)
    assert not destination.exists()
    assert not list(root.glob(case + "-*.tmp-*")), "Failed publication leaves no partial directory"


with tempfile.TemporaryDirectory(prefix="geocoin-db-backup-") as temporary:
    root = Path(temporary)
    source = root / "node"
    run(fixture, "--snapshot-fixture", str(source))
    for case in sidechain_valid:
        if selected is None or selected == case:
            sidechain_root = root / case
            sidechain_root.mkdir()
            sidechain_source = sidechain_root / "node"
            shutil.copytree(source, sidechain_source)
            run(fixture, "--alter-snapshot-fixture", str(sidechain_source), case)
            round_trip(sidechain_root, sidechain_source)
    if selected is None or selected == "card-valid":
        card_root = root / "card-roundtrip"
        card_root.mkdir()
        card_source = card_root / "node"
        shutil.copytree(source, card_source)
        run(fixture, "--alter-snapshot-fixture", str(card_source), "card-valid")
        round_trip(card_root, card_source)
    if selected is None:
        backup, restored = round_trip(root, source)
        run(tool, "restore", str(backup), str(restored), succeeds=False)
        broken = root / "broken"
        shutil.copytree(backup, broken)
        data_file = next((broken / "addresses/state").glob("*.sst"))
        with data_file.open("r+b") as stream:
            first = stream.read(1)
            stream.seek(0)
            stream.write(bytes([first[0] ^ 0x80]))
        run(tool, "verify", str(broken), succeeds=False)
        run(tool, "restore", str(broken), str(root / "invalid-restore"), succeeds=False)
        assert not (root / "invalid-restore").exists()
        if sys.platform != "win32":
            import fcntl
            with (source / "chaindata/blocks/LOCK").open("a+") as lock:
                fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                run(tool, "backup", str(source), str(root / "locked-backup"), succeeds=False)
            assert not (root / "locked-backup").exists()

    witness_source = root / "witness-node"
    if selected is None or selected in ("prune-witness", "payload-hash", "payload-data"):
        run(fixture, "--snapshot-witness-fixture", str(witness_source))
        run(fixture, "--alter-snapshot-fixture", str(witness_source), "prune-witness")
    for case in damage_cases:
        if selected is None or selected == case:
            seed = witness_source if case.startswith("payload-") else source
            reject_damaged(root, seed, case)
    for case in pruning_cases:
        if selected is None or selected == case:
            work = root / case
            work.mkdir()
            pruned = work / "node"
            shutil.copytree(witness_source if case == "prune-witness" else source, pruned)
            if case != "prune-witness":
                run(fixture, "--alter-snapshot-fixture", str(pruned), case)
            round_trip(work, pruned)
            if case == "prune-indexes":
                for corruption in ("missing", "duplicate", "length", "tag", "null-hash"):
                    reject_damaged(work, pruned, "geography-" + corruption)
print("Database backup, restore, integrity and lock checks passed")
