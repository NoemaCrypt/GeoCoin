#pragma once

#include "geocoin/core/block.hpp"

namespace geocoin::core {

enum class GenesisNetwork {
    MAINNET,
    TESTNET,
    LOADNET
};

struct GenesisParameters {
    const char* network_id;
    uint64_t timestamp;
    uint32_t bits;
    uint64_t nonce;
    const char* message_suffix;
    const char* foundation_address;
};

const GenesisParameters& genesis_parameters(GenesisNetwork network);
std::string genesis_message(GenesisNetwork network);

// Создать Genesis блок для конкретной сети.
Block create_genesis_block(GenesisNetwork network);

// Получить hardcoded хеш Genesis блока.
Hash256 get_genesis_hash(GenesisNetwork network);

// Проверить, является ли блок Genesis.
bool is_genesis_block(const Block& block, GenesisNetwork network);

// Сообщение генезиса.
constexpr const char* GENESIS_MESSAGE =
    "Primus exercitus in mundo est exercitus qui uxores, liberos matresque suas defendit! "
    "Exercitus qui aliis imponit ut uxores, liberos matresque eorum diligant, occupator est! "
    "The first army in the world is an army that protects its wives, children, and mothers! "
    "An army that imposes on others that they must love their wives, children, and mothers is an occupier! "
    "Первая армия в мире — это армия, защищающая своих жен, детей и матерей! "
    "Армия, навязывающая другим любовь к их женам, детям и матерям, является оккупантом!";

// Mainnet genesis: 2026-09-26 14:26:15 Europe/Minsk (UTC+03:00).
// Keep the exact Unix time in consensus source for reproducible mining.
constexpr uint64_t GENESIS_TIMESTAMP = 1790421975;
constexpr uint64_t TESTNET_GENESIS_TIMESTAMP = 1783987200;

// Координаты Genesis.
constexpr double GENESIS_BIRTHPLACE_LAT = 48.851583333333333;
constexpr double GENESIS_BIRTHPLACE_LON = 8.897277777777778;

} // namespace geocoin::core
