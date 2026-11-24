"""Documentation-vs-code synchronization gate.

Consensus constants drift out of the docs silently. This test parses the
real values from the C++ headers and verifies that the published documents
still state the same numbers, that the EN/RU document pairs exist and
cross-link each other, and that the docs index lists every pair.

Run standalone:
    python3 tests/test_documentation_sync.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

CONSENSUS_HPP = ROOT / "include/geocoin/consensus/consensus.hpp"
TYPES_HPP = ROOT / "include/geocoin/types.hpp"
POW_HPP = ROOT / "include/geocoin/consensus/proof_of_work.hpp"

INTEREST_POOL_EN = ROOT / "docs/INTEREST_POOL.md"
INTEREST_POOL_RU = ROOT / "docs/INTEREST_POOL_RU.md"
CAPABILITIES_EN = ROOT / "docs/BLOCKCHAIN_CAPABILITIES.md"
CAPABILITIES_RU = ROOT / "docs/BLOCKCHAIN_CAPABILITIES_RU.md"
EXCHANGE_INTEGRATION = ROOT / "docs/exchange-integration.txt"
DOCS_INDEX = ROOT / "docs/README.md"

PAIRS = [
    ("TECHNICAL_OVERVIEW.md", "TECHNICAL_OVERVIEW_RU.md"),
    ("ARCHITECTURE.md", "ARCHITECTURE_RU.md"),
    ("B2B_FOUNDATION.md", "B2B_FOUNDATION_RU.md"),
    ("INTEREST_POOL.md", "INTEREST_POOL_RU.md"),
    ("BLOCKCHAIN_CAPABILITIES.md", "BLOCKCHAIN_CAPABILITIES_RU.md"),
    ("PRODUCTION_TESTING.md", "PRODUCTION_TESTING_RU.md"),
]


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_const_scalar(text: str, name: str) -> int:
    """Return the integer value of `constexpr <type> NAME = <expr>;`."""
    match = re.search(
        r"constexpr\s+[\w:\s]+?\b" + re.escape(name) + r"\s*=\s*([^;]+);", text)
    if not match:
        raise AssertionError(f"constant {name} not found in headers")
    expr = match.group(1).strip()
    if not re.fullmatch(r"[0-9 \t()*+/-]+", expr):
        raise AssertionError(
            f"constant {name} has an unsupported expression: {expr!r}")
    return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - validated charset


def parse_const_string(text: str, name: str) -> str:
    match = re.search(
        r"constexpr\s+const char\*\s+" + re.escape(name) + r'\s*=\s*"([^"]+)"', text)
    if not match:
        raise AssertionError(f"string constant {name} not found in headers")
    return match.group(1)


def parse_const_name_list(text: str, name: str) -> list:
    match = re.search(
        r"constexpr\s+const char\*\s+" + re.escape(name) +
        r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if not match:
        raise AssertionError(f"array constant {name} not found in headers")
    return re.findall(r'"([^"]+)"', match.group(1))


def parse_return_constant(text: str, function: str) -> int:
    """Return the integer literal of `static ... <function>() { return <int>; }`."""
    match = re.search(
        r"\b" + re.escape(function) + r"\(\)\s*\{[^}]*?return\s+(\d+)\s*;", text)
    if not match:
        raise AssertionError(f"function constant {function} not found in headers")
    return int(match.group(1))


def normalize_numbers(text: str) -> str:
    """Collapse `210,000` / `210 000` / `210\\u00a0000` into `210000`."""
    return re.sub(r"(?<=[0-9])[ \u00a0,](?=[0-9])", "", text)


class HeaderConstants(unittest.TestCase):
    """The parsed source of truth must contain the advertised values."""

    @classmethod
    def setUpClass(cls):
        cls.consensus = read(CONSENSUS_HPP)
        cls.pow = read(POW_HPP)

    def test_consensus_constants(self):
        cases = {
            "TARGET_BLOCK_TIME": 600,
            "DIFFICULTY_DAA_WINDOW": 60,
            "MAX_FUTURE_BLOCK_TIME": 900,
            "MEDIAN_TIME_SPAN": 11,
            "MAX_REORG_DEPTH": 144,
            "COINBASE_MATURITY": 100,
            "DUST_LIMIT": 1000,
            "MAX_TX_OUTPUTS": 1000,
            "FOUNDATION_M": 3,
            "FOUNDATION_N": 4,
            "FOUNDATION_REWARD_PERCENT": 10,
            "INTEREST_POOL_ACTIVATION_HEIGHT": 210000,
            "INTEREST_DISTRIBUTION_INTERVAL": 13104,
        }
        for name, expected in cases.items():
            with self.subTest(name=name):
                self.assertEqual(parse_const_scalar(self.consensus, name), expected)

    def test_reward_and_halving(self):
        coin = re.search(r"\bCOIN\s*=\s*(\d+)", read(TYPES_HPP))
        self.assertIsNotNone(coin, "COIN constant not found in consensus.hpp")
        reward = re.search(
            r"get_initial_reward\(\)\s*\{[^}]*?return\s+([^;]+);", self.pow)
        self.assertIsNotNone(reward, "get_initial_reward body not found")
        expr = reward.group(1).strip()
        match = re.fullmatch(r"(\d+)\s*\*\s*COIN", expr)
        self.assertIsNotNone(match, f"unexpected reward expression: {expr!r}")
        self.assertEqual(int(match.group(1)), 50, "initial reward must be 50 GEO")
        self.assertEqual(
            parse_return_constant(self.pow, "get_halving_interval"), 210000)

    def test_foundation_addresses(self):
        address = parse_const_string(self.consensus, "FOUNDATION_ADDRESS")
        self.assertTrue(address.startswith("GEO"), address)
        participants = parse_const_name_list(self.consensus, "FOUNDATION_PARTICIPANTS")
        self.assertEqual(
            len(participants), parse_const_scalar(self.consensus, "FOUNDATION_N"))


class InterestPoolDocs(unittest.TestCase):
    """docs/INTEREST_POOL{,_RU}.md must restate the consensus numbers."""

    def setUp(self):
        self.docs = {
            "en": normalize_numbers(read(INTEREST_POOL_EN)),
            "ru": normalize_numbers(read(INTEREST_POOL_RU)),
        }

    def expect(self, en_pattern=None, ru_pattern=None):
        checks = [("en", en_pattern), ("ru", ru_pattern or en_pattern)]
        for doc, pattern in checks:
            if pattern is None:
                continue
            with self.subTest(doc=doc, pattern=pattern):
                self.assertRegex(self.docs[doc], pattern)

    def test_core_numbers(self):
        self.expect(r"210000")
        self.expect(r"13104")
        self.expect(r"50 GEO")
        self.expect(r"25 GEO")
        self.expect(r"10%")
        self.expect(r"0\.00001 GC")
        self.expect(r"1000")

    def test_foundation_policy(self):
        address = parse_const_string(read(CONSENSUS_HPP), "FOUNDATION_ADDRESS")
        for doc, text in self.docs.items():
            with self.subTest(doc=doc):
                self.assertIn(address, text)
        self.expect(en_pattern=r"3-of-4", ru_pattern=r"3 из 4")

    def test_limitation_is_disclosed(self):
        for doc, text in self.docs.items():
            with self.subTest(doc=doc):
                self.assertIn("MAX_TX_OUTPUTS", text)
                self.assertRegex(text, r"[Pp]lan|[Пп]лан")


class CapabilitiesDocs(unittest.TestCase):
    """docs/BLOCKCHAIN_CAPABILITIES{,_RU}.md must restate L1 parameters."""

    def setUp(self):
        self.docs = {
            "en": normalize_numbers(read(CAPABILITIES_EN)),
            "ru": normalize_numbers(read(CAPABILITIES_RU)),
        }

    def expect(self, en_pattern=None, ru_pattern=None):
        checks = [("en", en_pattern), ("ru", ru_pattern or en_pattern)]
        for doc, pattern in checks:
            if pattern is None:
                continue
            with self.subTest(doc=doc, pattern=pattern):
                self.assertRegex(self.docs[doc], pattern)

    def test_consensus_parameters(self):
        self.expect(r"TARGET_BLOCK_TIME = 600")
        self.expect(r"DIFFICULTY_DAA_WINDOW = 60")
        self.expect(r"MAX_FUTURE_BLOCK_TIME = 15")
        self.expect(r"MAX_REORG_DEPTH = 144")
        self.expect(r"COINBASE_MATURITY = 100")
        self.expect(r"50 GC")
        self.expect(r"210000")
        self.expect(r"13104")

    def test_median_time_past_span(self):
        self.expect(en_pattern=r"MTP.*?11|11 blocks",
                    ru_pattern=r"MTP.*?11|11 блоков")

    def test_exchange_doc_mentions_node_ports(self):
        text = read(EXCHANGE_INTEGRATION)
        self.assertIn("3030", text, "P2P port missing from exchange doc")
        self.assertIn("3033", text, "RPC port missing from exchange doc")


class LanguagePairs(unittest.TestCase):
    """Every published document pair must exist and cross-link."""

    def test_pairs_exist_and_cross_link(self):
        for en_name, ru_name in PAIRS:
            with self.subTest(pair=f"{en_name} <-> {ru_name}"):
                en_path, ru_path = ROOT / "docs" / en_name, ROOT / "docs" / ru_name
                self.assertTrue(en_path.exists(), f"missing {en_path}")
                self.assertTrue(ru_path.exists(), f"missing {ru_path}")
                en_text, ru_text = read(en_path), read(ru_path)
                self.assertIn(ru_name, en_text, f"{en_name} must link {ru_name}")
                self.assertIn(en_name, ru_text, f"{ru_name} must link {en_name}")

    def test_index_lists_pairs(self):
        index = read(DOCS_INDEX)
        for en_name, ru_name in PAIRS:
            with self.subTest(index=en_name):
                self.assertIn(en_name, index, f"docs/README.md must list {en_name}")
            with self.subTest(index=ru_name):
                self.assertIn(ru_name, index, f"docs/README.md must list {ru_name}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
