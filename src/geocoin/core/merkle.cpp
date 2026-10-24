#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/geo_area.hpp"

#include <cstring>

namespace geocoin::core {

namespace {

Hash256 hash_merkle_pair(const Hash256& left, const Hash256& right) {
    uint8_t combined[64];
    std::memcpy(combined, left.data(), 32);
    std::memcpy(combined + 32, right.data(), 32);
    return crypto::double_sha256(combined, sizeof(combined));
}

} // namespace

Hash256 compute_merkle_root(const std::vector<Hash256>& hashes) {
    if (hashes.empty()) {
        bytes empty;
        return crypto::sha256(empty);
    }

    if (hashes.size() == 1) {
        return hashes[0];
    }

    std::vector<Hash256> current_level = hashes;

    while (current_level.size() > 1) {
        std::vector<Hash256> next_level;

        for (size_t i = 0; i < current_level.size(); i += 2) {
            const Hash256& right = (i + 1 < current_level.size())
                ? current_level[i + 1]
                : current_level[i];
            next_level.push_back(hash_merkle_pair(current_level[i], right));
        }

        current_level = next_level;
    }

    return current_level[0];
}

std::vector<Hash256> compute_merkle_path(const std::vector<Hash256>& hashes, size_t index) {
    std::vector<Hash256> path;

    if (index >= hashes.size()) {
        return path;
    }

    std::vector<Hash256> current_level = hashes;
    size_t current_index = index;

    while (current_level.size() > 1) {
        // Добавляем sibling в путь
        if (current_index % 2 == 0) {
            // Левый узел - добавляем правый
            if (current_index + 1 < current_level.size()) {
                path.push_back(current_level[current_index + 1]);
            } else {
                path.push_back(current_level[current_index]);
            }
        } else {
            // Правый узел - добавляем левый
            path.push_back(current_level[current_index - 1]);
        }

        // Переходим на следующий уровень
        std::vector<Hash256> next_level;
        for (size_t i = 0; i < current_level.size(); i += 2) {
            const Hash256& right = (i + 1 < current_level.size())
                ? current_level[i + 1]
                : current_level[i];
            next_level.push_back(hash_merkle_pair(current_level[i], right));
        }

        current_level = next_level;
        current_index /= 2;
    }

    return path;
}

bool verify_merkle_path(const Hash256& tx_hash,
                       const std::vector<Hash256>& merkle_path,
                       const Hash256& merkle_root,
                       size_t index) {
    Hash256 current_hash = tx_hash;
    size_t current_index = index;

    for (const auto& sibling : merkle_path) {
        current_hash = (current_index % 2 == 0)
            ? hash_merkle_pair(current_hash, sibling)
            : hash_merkle_pair(sibling, current_hash);
        current_index /= 2;
    }

    return current_hash == merkle_root;
}

} // namespace geocoin::core
