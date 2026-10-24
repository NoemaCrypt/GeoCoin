#pragma once

#include "geocoin/types.hpp"

#include <string>
#include <vector>

namespace geocoin::utils {

struct FeatureActivation {
    bool enabled = false;
    Height activation_height = 0;
};

struct ActivationManifest {
    int manifest_version = 1;
    std::string network = "degree";
    std::string profile = "production";
    bool external_audit_required = true;
    bool external_audit_signed_off = false;
    bool internal_signoff_signed_off = false;
    std::vector<std::string> internal_signers;
    std::string internal_audited_commit;
    std::string internal_signed_at_utc;

    FeatureActivation standard_multisig{true, 0};
    FeatureActivation sidechain{true, 0};
    FeatureActivation hierarchical_multisig{};
    FeatureActivation delegation{};
    FeatureActivation nft{};
    FeatureActivation cadastral{};

    static ActivationManifest production_defaults(const std::string& network = "degree");
    static ActivationManifest fully_enabled_for_tests(const std::string& network = "degree");

    bool is_internal_signoff_valid() const;
    bool is_external_audit_gate_open() const;
    bool is_security_gate_open() const;
    bool is_standard_multisig_active(Height height) const;
    bool is_sidechain_active(Height height) const;
    bool is_hierarchical_multisig_active(Height height) const;
    bool is_delegation_active(Height height) const;
    bool is_nft_active(Height height) const;
    bool is_cadastral_active(Height height) const;

    bool has_risky_features_enabled() const;
    void validate_or_throw() const;

    std::string to_json_string(int indent = 2) const;
};

ActivationManifest read_activation_manifest(const std::string& path);
bool write_activation_manifest(const std::string& path, const ActivationManifest& manifest);
ActivationManifest ensure_activation_manifest(
    const std::string& path,
    const std::string& network,
    bool create_if_missing = true
);

} // namespace geocoin::utils
