#include "geocoin/core/block.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"

#include <cstdlib>
#include <exception>
#include <new>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace geocoin;
    if (size == 0) return 0;
    const bytes payload(data + 1, data + size);
    try {
        switch (data[0] % 4) {
        case 0: {
            size_t offset = 0;
            const auto tx = core::Transaction::deserialize(payload, offset);
            if (offset > payload.size()) std::abort();
            const auto encoded = tx.serialize();
            offset = 0;
            const auto roundtrip = core::Transaction::deserialize(encoded, offset);
            if (offset != encoded.size() || tx.get_hash() != roundtrip.get_hash()) std::abort();
            break;
        }
        case 1: (void)core::Block::deserialize(payload); break;
        case 2: {
            core::BatchCheckpointItem item;
            item.proof = payload;
            (void)consensus::sidechain_checkpoint::verify_financial_v2(item, COIN, 0);
            break;
        }
        case 3: {
            core::BatchCheckpointItem item;
            item.proof = payload;
            (void)consensus::sidechain_checkpoint::verify_accounting_v1(item, {});
            break;
        }
        }
    } catch (const std::bad_alloc&) {
        throw; // Allocation amplification is a finding, not a parse rejection.
    } catch (const std::exception&) {
        // Invalid serialized transactions/blocks are rejected with exceptions.
    }
    return 0;
}
