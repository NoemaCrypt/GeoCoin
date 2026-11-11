#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/types.hpp"

#include <cstddef>
#include <set>
#include <stdexcept>
#include <vector>

namespace geocoin::multisig {

/** Participant in an M-of-N Falcon multisig policy. */
struct MultisigParticipant {
    Address address;

    MultisigParticipant() = default;
    explicit MultisigParticipant(const Address& addr) : address(addr) {}
};

/** Consensus-visible M-of-N policy. */
struct MultisigConfig {
    uint8_t m;
    uint8_t n;
    Address address;
    std::vector<Address> addresses;
    std::vector<MultisigParticipant> participants;

    MultisigConfig() : m(0), n(0) {}
    MultisigConfig(uint8_t m_, uint8_t n_, const std::vector<Address>& addrs)
        : m(m_), n(n_), addresses(addrs) {
        if (m > n) {
            throw std::invalid_argument("M cannot be greater than N");
        }
        if (n > 15) {
            throw std::invalid_argument("N cannot be greater than 15");
        }
        if (m == 0) {
            throw std::invalid_argument("M must be at least 1");
        }
        if (addresses.size() != n) {
            throw std::invalid_argument("Number of addresses must equal N");
        }
    }

    bool is_valid() const {
        return m > 0 && m <= n && n <= 15 &&
            (addresses.size() == n || participants.size() == n);
    }
};

struct MultisigAddress {
    AddressString address;
    MultisigConfig config;

    MultisigAddress() = default;
    MultisigAddress(const AddressString& addr, const MultisigConfig& cfg)
        : address(addr), config(cfg) {}

    bool is_valid() const {
        return !address.empty() && config.is_valid();
    }
};

MultisigAddress create_multisig_address(
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& addresses
);

bool validate_multisig_config(
    const Address& expected_address,
    const MultisigConfig& config
);

bool is_multisig_address(const AddressString& address);
MultisigConfig decode_multisig_address(const AddressString& address);

struct PartialSignature {
    Address signer_address;
    size_t input_index;
    Signature signature;

    PartialSignature() : input_index(0) {}
    PartialSignature(const Address& addr, size_t idx, const Signature& sig)
        : signer_address(addr), input_index(idx), signature(sig) {}
};

} // namespace geocoin::multisig
