#include "geocoin/state/state_serialization_internal.hpp"

#include "geocoin/utils/bytes.hpp"

namespace geocoin::state {

std::string UTXO::get_key() const {
    std::string key = "utxo:";
    key.reserve(80); // Prefix + 64 hex digits + ':' + at most 10 decimal digits.
    key += utils::hash256_to_hex(tx_hash);
    key += ':';
    key += std::to_string(vout);
    return key;
}

std::string UTXO::address_index_prefix(const Address& address) {
    const auto& text = address.str();
    return "addr:utxo:" + utils::bytes_to_hex(reinterpret_cast<const byte*>(text.data()), text.size()) + ":";
}

std::string UTXO::get_address_index_key() const {
    return address_index_prefix(address) + get_key();
}

bytes UTXO::serialize() const {
    bytes data;

    if (value < 0) {
        throw std::runtime_error("UTXO: value must be non-negative");
    }
    const std::string& addr_str = address.address;
    if (addr_str.size() > consensus::params::MAX_ADDRESS_SIZE) {
        throw std::runtime_error("UTXO: invalid address length");
    }
    data.reserve(54 + addr_str.size());

    // tx_hash (32 байта)
    data.insert(data.end(), tx_hash.begin(), tx_hash.end());

    // vout (4 байта)
    for (int i = 0; i < 4; i++) {
        data.push_back((vout >> (i * 8)) & 0xFF);
    }

    // value (8 байт)
    for (int i = 0; i < 8; i++) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }

    // address (переменная длина, максимум 64)
    uint8_t addr_len = addr_str.size();
    data.push_back(addr_len);
    data.insert(data.end(), addr_str.begin(), addr_str.end());

    // height (8 байт)
    for (int i = 0; i < 8; i++) {
        data.push_back((height >> (i * 8)) & 0xFF);
    }

    // is_coinbase (1 байт)
    data.push_back(is_coinbase ? 1 : 0);

    return data;
}

UTXO UTXO::deserialize(const bytes& data) {
    ByteReader reader(data, "UTXO");
    UTXO utxo;

    // tx_hash (32 байта)
    utxo.tx_hash = reader.read_hash256("tx_hash");

    // vout (4 байта)
    utxo.vout = reader.read_u32_le("vout");

    // value (8 байт)
    const uint64_t raw_value = reader.read_u64_le("value");
    if (raw_value > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        throw std::runtime_error("UTXO: invalid value");
    }
    utxo.value = static_cast<Amount>(raw_value);

    // строка адреса
    uint8_t addr_len = reader.read_u8("address length");
    if (addr_len > consensus::params::MAX_ADDRESS_SIZE) {
        throw std::runtime_error("UTXO: invalid address length");
    }
    std::string addr_str = reader.read_string(addr_len, "address");

    // Адрес хранится консенсусно как строка.
    utxo.address = Address(addr_str);

    // height (8 байт)
    utxo.height = reader.read_u64_le("height");

    // is_coinbase (1 байт)
    const uint8_t coinbase_flag = reader.read_u8("is_coinbase");
    if (coinbase_flag > 1) {
        throw std::runtime_error("UTXO: invalid coinbase flag");
    }
    utxo.is_coinbase = (coinbase_flag == 1);

    if (!reader.empty()) {
        throw std::runtime_error("UTXO: trailing data");
    }

    return utxo;
}

} // namespace geocoin::state
