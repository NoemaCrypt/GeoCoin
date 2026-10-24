#include "geocoin/multisig/multisig.hpp"
#include "geocoin/crypto/addresses.hpp"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <set>

namespace geocoin::multisig {

namespace {
    // Версия для мультиподпись адресов
    constexpr uint8_t MULTISIG_VERSION = 0x05;  // Отличается от обычных адресов (0x00)

    // Префикс для мультиподпись адресов
    constexpr const char* MULTISIG_PREFIX = "GEOM";  // GEO + M (мультисиг)
}

MultisigAddress create_multisig_address(
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& addresses
) {
    MultisigConfig config(m, n, addresses);

    // Создаем хеш конфигурации
    bytes config_data;
    config_data.push_back(m);
    config_data.push_back(n);

    // Добавляем все адреса (сортированные для детерминизма)
    std::vector<AddressString> sorted_addrs;
    for (const auto& addr : addresses) {
        sorted_addrs.push_back(addr.address);
    }
    std::sort(sorted_addrs.begin(), sorted_addrs.end());

    for (const auto& addr : sorted_addrs) {
        bytes addr_bytes(addr.begin(), addr.end());
        config_data.insert(config_data.end(), addr_bytes.begin(), addr_bytes.end());
    }

    // Хешируем конфигурацию
    Hash256 config_hash = crypto::sha256(config_data);
    Hash160 config_hash160 = crypto::ripemd160(bytes(config_hash.begin(), config_hash.end()));

    // Создаем адрес: version + hash160 + checksum
    bytes addr_data;
    addr_data.push_back(MULTISIG_VERSION);
    addr_data.insert(addr_data.end(), config_hash160.begin(), config_hash160.end());

    // Вычисляем checksum (двойной SHA-256)
    Hash256 checksum_full = crypto::sha256(addr_data);
    Hash256 checksum_hash = crypto::sha256(bytes(checksum_full.begin(), checksum_full.end()));
    addr_data.insert(addr_data.end(), checksum_hash.begin(), checksum_hash.begin() + 4);

    // Кодируем в Base58
    std::string base58_addr = crypto::base58_encode(addr_data);

    // Добавляем префикс
    AddressString multisig_addr = MULTISIG_PREFIX + base58_addr;

    return MultisigAddress(multisig_addr, config);
}

bool validate_multisig_config(
    const Address& expected_address,
    const MultisigConfig& config
) {
    if (!config.is_valid() || !is_multisig_address(expected_address.str())) {
        return false;
    }

    std::vector<Address> addresses;
    if (!config.participants.empty()) {
        addresses.reserve(config.participants.size());
        for (const auto& participant : config.participants) {
            addresses.push_back(participant.address);
        }
    } else {
        addresses = config.addresses;
    }
    if (addresses.size() != config.n) {
        return false;
    }

    std::set<std::string> unique;
    for (const auto& address : addresses) {
        if (!crypto::verify_address(address) || !unique.insert(address.str()).second) {
            return false;
        }
    }

    if (!config.addresses.empty()) {
        std::vector<std::string> legacy;
        legacy.reserve(config.addresses.size());
        for (const auto& address : config.addresses) {
            legacy.push_back(address.str());
        }
        std::vector<std::string> canonical;
        canonical.reserve(addresses.size());
        for (const auto& address : addresses) {
            canonical.push_back(address.str());
        }
        std::sort(legacy.begin(), legacy.end());
        std::sort(canonical.begin(), canonical.end());
        if (legacy != canonical) {
            return false;
        }
    }

    try {
        return create_multisig_address(config.m, config.n, addresses).address == expected_address.str();
    } catch (const std::exception&) {
        return false;
    }
}

bool is_multisig_address(const AddressString& address) {
    return address.size() > 4 &&
           address.substr(0, 4) == MULTISIG_PREFIX;
}

MultisigConfig decode_multisig_address(const AddressString& address) {
    if (!is_multisig_address(address)) {
        throw std::invalid_argument("Not a multisig address");
    }

    // Удаляем префикс
    std::string base58_part = address.substr(4);

    // Декодируем Base58
    bytes addr_data = crypto::base58_decode(base58_part);

    if (addr_data.size() != 25) {  // версия(1) + hash160(20) + checksum(4)
        throw std::invalid_argument("Invalid multisig address format");
    }

    // Проверяем checksum
    bytes payload(addr_data.begin(), addr_data.end() - 4);
    Hash256 checksum_full = crypto::sha256(payload);
    Hash256 checksum_hash = crypto::sha256(bytes(checksum_full.begin(), checksum_full.end()));

    if (std::memcmp(checksum_hash.data(), &addr_data[21], 4) != 0) {
        throw std::invalid_argument("Invalid checksum");
    }

    // Проверяем версию
    if (addr_data[0] != MULTISIG_VERSION) {
        throw std::invalid_argument("Invalid multisig version");
    }

    // ПРИМЕЧАНИЕ: Мы не можем восстановить полную конфигурацию из адреса!
    // Адрес содержит только хеш конфигурации.
    // Полная конфигурация должна храниться отдельно (в кошельке).

    // Возвращаем пустую конфигурацию (нужно загрузить из кошелька)
    return MultisigConfig();
}

} // namespace geocoin::multisig
