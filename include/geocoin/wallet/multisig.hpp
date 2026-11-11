#pragma once

// Compatibility facade. Consensus-visible multisig types belong to the
// geocoin::multisig domain; wallet callers retain their historical names.
#include "geocoin/multisig/multisig.hpp"

namespace geocoin::wallet {

using multisig::MultisigParticipant;
using multisig::MultisigConfig;
using multisig::MultisigAddress;
using multisig::PartialSignature;
using multisig::create_multisig_address;
using multisig::validate_multisig_config;
using multisig::is_multisig_address;
using multisig::decode_multisig_address;

} // namespace geocoin::wallet
