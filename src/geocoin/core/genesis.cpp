#include "geocoin/core/genesis.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include <stdexcept>

namespace geocoin::core {

namespace {
constexpr GenesisParameters kMainnetGenesis{
    "degree",
    GENESIS_TIMESTAMP,
    0x1d00ffff,
    2799726649ULL,
    " Network: degree.",
    consensus::params::FOUNDATION_ADDRESS
};

constexpr GenesisParameters kTestnetGenesis{
    "degree-test",
    TESTNET_GENESIS_TIMESTAMP,
    0x207fffff,
    0,
    " Network: degree-test.",
    consensus::params::TESTNET_FOUNDATION_ADDRESS
};

constexpr GenesisParameters kLoadnetGenesis{
    "degree-load",
    1786752000ULL,  // 2026-08-15 00:00:00 UTC
    0x207fffff,
    0,
    " Network: degree-load. Foundation: isolated deterministic 4-of-4.",
    consensus::params::LOADNET_FOUNDATION_ADDRESS
};
} // namespace

const GenesisParameters& genesis_parameters(GenesisNetwork network) {
    switch (network) {
        case GenesisNetwork::MAINNET:
            return kMainnetGenesis;
        case GenesisNetwork::TESTNET:
            return kTestnetGenesis;
        case GenesisNetwork::LOADNET:
            return kLoadnetGenesis;
    }
    throw std::invalid_argument("unknown genesis network");
}

std::string genesis_message(GenesisNetwork network) {
    const auto& params = genesis_parameters(network);
    return std::string(GENESIS_MESSAGE) + params.message_suffix;
}

Block create_genesis_block(GenesisNetwork network) {
    const auto& params = genesis_parameters(network);
    Block genesis;

    // Заголовок
    genesis.header.version = 1;
    genesis.header.timestamp = static_cast<uint32_t>(params.timestamp);
    genesis.header.prev_block_hash = {};  // Все нули
    genesis.header.bits = params.bits;

    // GeoPoW тег для genesis фиксирован консенсусом.
    genesis.header.geo_tag = consensus::geo::GENESIS_GEO_TAG;

    // Any change to these committed fields requires independently re-mining
    // the corresponding network and updating get_genesis_hash().
    genesis.header.nonce = params.nonce;

    // Coinbase транзакция с сообщением
    // Используем адрес фонда для Genesis блока
    auto coinbase = Transaction::create_coinbase(
        0,  // высота
        Address(params.foundation_address),
        consensus::ProofOfWork::calculate_block_reward(0),
        0,  // комиссии
        Address(params.foundation_address)
    );
    coinbase.timestamp = params.timestamp;

    // Добавляем Genesis сообщение в script_sig
    const std::string message = genesis_message(network);

    coinbase.inputs[0].script_sig.clear();
    coinbase.inputs[0].script_sig.insert(
        coinbase.inputs[0].script_sig.end(),
        reinterpret_cast<const byte*>(message.data()),
        reinterpret_cast<const byte*>(message.data()) + message.size()
    );

    genesis.transactions.push_back(coinbase);

    // Вычисляем Merkle root
    genesis.calculate_merkle_root();

    // Witness commitment входит в PoW-хеш заголовка, поэтому фиксируем его в Genesis.
    genesis.header.witness_commitment = genesis.calculate_witness_commitment();

    return genesis;
}

Hash256 get_genesis_hash(GenesisNetwork network) {
    switch (network) {
        case GenesisNetwork::MAINNET:
            return Hash256{
                0x00, 0x00, 0x00, 0x00, 0xe2, 0xc7, 0x0b, 0x6a,
                0x2e, 0x23, 0xda, 0xbe, 0xe7, 0x68, 0x68, 0xef,
                0xfa, 0x1d, 0x10, 0xaa, 0xf7, 0xa9, 0x65, 0xca,
                0xb0, 0x5b, 0x88, 0x8b, 0xb4, 0x3a, 0x28, 0xb7
            };
        case GenesisNetwork::TESTNET:
            return Hash256{
                0x62, 0x58, 0x3e, 0x09, 0x57, 0x47, 0x92, 0x57,
                0x4d, 0x8c, 0xfc, 0xe9, 0x66, 0xf3, 0x28, 0x1a,
                0xb9, 0x7e, 0xb7, 0x26, 0x5f, 0x41, 0xcd, 0x14,
                0xdc, 0xce, 0xab, 0x2e, 0xfb, 0x32, 0xf1, 0x38
            };
        case GenesisNetwork::LOADNET:
            return Hash256{
                0x63, 0x56, 0x97, 0x01, 0xef, 0xc8, 0xd9, 0x50,
                0xc0, 0xee, 0x1b, 0xfc, 0xa3, 0x0d, 0x69, 0x2d,
                0xd1, 0xff, 0x00, 0x64, 0x7f, 0x5c, 0x48, 0xc5,
                0x15, 0xad, 0xd8, 0xa6, 0x15, 0xf7, 0x06, 0x75
            };
    }
    throw std::invalid_argument("unknown genesis network");
}

bool is_genesis_block(const Block& block, GenesisNetwork network) {
    return block.get_hash() == get_genesis_hash(network);
}

} // namespace geocoin::core
