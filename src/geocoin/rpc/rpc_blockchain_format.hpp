#pragma once

#include "geocoin/rpc/rpc_types.hpp"

namespace geocoin::core {
class Block;
class Blockchain;
class Transaction;
}
namespace geocoin::state { class ChainState; }

namespace geocoin::rpc {

json block_to_verbose_json(
    const core::Block& block,
    const Hash256& block_hash,
    const std::string& block_hash_hex,
    const core::Blockchain& blockchain
);

json transaction_to_verbose_json(
    const core::Transaction& tx,
    const Hash256& txid,
    const std::string& txid_hex,
    const state::ChainState& chain_state
);

json delegation_proof_to_json(
    const core::Transaction& tx,
    const Hash256& txid,
    const std::string& txid_hex,
    const state::ChainState& chain_state
);

} // namespace geocoin::rpc
