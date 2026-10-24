#pragma once
#include "geocoin/types.hpp"

namespace geocoin::state {
struct SidechainDeposit;
struct SidechainRegistryEntry;
struct CardProgramEntry;
struct CardInstanceEntry;
struct FoundationConfig;
struct DelegationState;

// One codec per current database record, shared by runtime and offline checks.
namespace records {
bytes serialize_sidechain_deposit(const SidechainDeposit&);
bool deserialize_sidechain_deposit(const bytes&, SidechainDeposit&);
bytes serialize_sidechain_registry(const SidechainRegistryEntry&);
bool deserialize_sidechain_registry(const bytes&, SidechainRegistryEntry&);
bool validate_sidechain_deposit(const SidechainDeposit&, Height tip_height);
bool validate_sidechain_registry(const SidechainRegistryEntry&, Height tip_height);
bytes serialize_card_program(const CardProgramEntry&);
bool deserialize_card_program(const bytes&, CardProgramEntry&);
bytes serialize_card_instance(const CardInstanceEntry&);
bool deserialize_card_instance(const bytes&, CardInstanceEntry&);
// Context checks shared by startup and offline snapshots. The instance's
// program must first pass validate_card_program and match its program_id key.
bool validate_card_program(const CardProgramEntry&, Height tip_height);
bool validate_card_instance(const CardInstanceEntry&, const CardProgramEntry&, Height tip_height);
bytes serialize_foundation_config(const FoundationConfig&);
bool deserialize_foundation_config(const bytes&, FoundationConfig&);
bool validate_foundation_config(const FoundationConfig&);
bytes serialize_delegation(const DelegationState&);
bool deserialize_delegation(const bytes&, DelegationState&);
}
}
