"""Negative tests keep the architecture gate from silently accepting violations."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "architecture", Path(__file__).resolve().parents[1] / "scripts/check_architecture.py")
architecture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(architecture)


class Boundaries(unittest.TestCase):
    def check_sources(self, sources):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "include/geocoin").mkdir(parents=True)
            (root / "src/geocoin").mkdir(parents=True)
            for name, content in sources.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            return architecture.check(root)

    def test_consensus_cannot_depend_on_wallet(self):
        self.assertTrue(self.check_sources({
            "src/geocoin/consensus/validation.cpp": '#include "geocoin/wallet/wallet.hpp"\n'}))

    def test_rpc_uses_node_interface(self):
        self.assertFalse(self.check_sources({
            "src/geocoin/rpc/rpc.cpp": '#include "geocoin/node/node_services.hpp"\n'}))
        self.assertTrue(self.check_sources({
            "src/geocoin/rpc/rpc.cpp": '#include "geocoin/node/chain_coordinator.hpp"\n'}))

    def test_public_header_cannot_expose_private_header(self):
        self.assertTrue(self.check_sources({
            "include/geocoin/state/state.hpp": '#include "geocoin/state/state_internal.hpp"\n',
            "src/geocoin/state/state_internal.hpp": "#pragma once\n"}))

    def test_rpc_uses_nft_value_types_only(self):
        self.assertFalse(self.check_sources({
            "src/geocoin/rpc/metadata.cpp": '#include "geocoin/nft/nft_types.hpp"\n'}))
        self.assertTrue(self.check_sources({
            "src/geocoin/rpc/metadata.cpp": '#include "geocoin/nft/geo_projection.hpp"\n'}))
        self.assertTrue(self.check_sources({
            "src/geocoin/rpc/metadata.cpp": '#include "../nft/nft_internal.hpp"\n',
            "src/geocoin/nft/nft_internal.hpp": "#pragma once\n"}))

    def test_relative_include_cannot_bypass_private_boundary(self):
        self.assertTrue(self.check_sources({
            "src/geocoin/wallet/wallet.cpp": '#include "../state/state_internal.hpp"\n',
            "src/geocoin/state/state_internal.hpp": "#pragma once\n"}))

    def test_blockchain_is_separate_from_value_types(self):
        self.assertFalse(self.check_sources({
            "src/geocoin/core/blockchain_reorg.cpp": '#include "geocoin/state/state.hpp"\n'}))
        self.assertTrue(self.check_sources({
            "src/geocoin/core/transaction.cpp": '#include "geocoin/state/state.hpp"\n'}))


if __name__ == "__main__":
    unittest.main()
