#include "geocoin/core/transaction.hpp"
#include "geocoin/consensus/consensus.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::Hash256 canonical_hash_from_serialized_checked(const geocoin::bytes& raw) {
    using geocoin::core::Transaction;
    size_t offset = 0;
    const Transaction tx = Transaction::deserialize(raw, offset);
    if (offset != raw.size()) {
        throw std::runtime_error("trailing bytes after tx");
    }
    return tx.get_hash();
}
}

int main() {
    using namespace geocoin;
    using namespace geocoin::core;

    try {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::STANDARD;
        tx.timestamp = 1710000000;
        tx.lock_time = 42;

        TxInput input;
        input.prev_tx_hash[0] = 0x12;
        input.prev_tx_hash[31] = 0x34;
        input.vout = 7;
        input.script_sig = bytes{0x51, 0x21};
        tx.inputs.push_back(input);

        TxOutput output;
        output.value = 5 * COIN;
        output.address = Address("GEOtestRecipient");
        output.script_pubkey = bytes{0x76, 0xA9, 0x88, 0xAC};
        tx.outputs.push_back(output);

        TxWitness witness;
        witness.public_key = bytes(consensus::params::WITNESS_PUBKEY_SIZE, 0x01);
        witness.public_key_hash = crypto::sha256(witness.public_key);
        witness.signature = bytes{0x05, 0x06, 0x07, 0x08};
        tx.witnesses.push_back(witness);

        const auto canonical_from_object = tx.get_hash();
        const auto serialized = tx.serialize();
        const auto canonical_from_raw = canonical_hash_from_serialized_checked(serialized);
        expect(canonical_from_raw == canonical_from_object, "raw/object txid mismatch");

        Transaction witness_modified = tx;
        witness_modified.witnesses[0].signature = bytes{0x09, 0x0A, 0x0B};
        const auto modified_raw = canonical_hash_from_serialized_checked(witness_modified.serialize());
        expect(modified_raw == canonical_from_object, "witness changed canonical txid");

        bool trailing_rejected = false;
        auto malformed = serialized;
        malformed.push_back(0x00);
        try {
            (void)canonical_hash_from_serialized_checked(malformed);
        } catch (const std::exception&) {
            trailing_rejected = true;
        }
        expect(trailing_rejected, "trailing bytes were not rejected");

        std::cout << "test_txid_contract: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_txid_contract failed: " << e.what() << "\n";
        return 1;
    }
}
