#include "geocoin/utils/activation_manifest.hpp"

#include "nlohmann_json.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace geocoin::utils {

namespace {
using json = nlohmann::json;

FeatureActivation parse_feature(const json& features, const char* key, bool default_enabled) {
    FeatureActivation result;
    result.enabled = default_enabled;
    result.activation_height = 0;

    if (!features.contains(key)) {
        return result;
    }

    const auto& item = features.at(key);
    result.enabled = item.value("enabled", default_enabled);
    result.activation_height = item.value("activation_height", Height{0});
    return result;
}

bool is_active(const FeatureActivation& feature, Height height) {
    return feature.enabled && height >= feature.activation_height;
}

json feature_to_json(const FeatureActivation& feature) {
    return json{
        {"enabled", feature.enabled},
        {"activation_height", feature.activation_height},
    };
}
} // namespace

ActivationManifest ActivationManifest::production_defaults(const std::string& network_id) {
    ActivationManifest manifest;
    manifest.network = network_id;
    manifest.profile = "production";
    manifest.external_audit_required = true;
    manifest.external_audit_signed_off = false;
    manifest.internal_signoff_signed_off = false;
    manifest.internal_signers.clear();
    manifest.internal_audited_commit.clear();
    manifest.internal_signed_at_utc.clear();
    manifest.standard_multisig = FeatureActivation{true, 0};
    manifest.sidechain = FeatureActivation{true, 0};
    manifest.hierarchical_multisig = FeatureActivation{false, 0};
    manifest.delegation = FeatureActivation{false, 0};
    manifest.nft = FeatureActivation{false, 0};
    manifest.cadastral = FeatureActivation{false, 0};
    return manifest;
}

ActivationManifest ActivationManifest::fully_enabled_for_tests(const std::string& network_id) {
    ActivationManifest manifest = production_defaults(network_id);
    manifest.profile = "test";
    manifest.external_audit_required = false;
    manifest.external_audit_signed_off = true;
    manifest.internal_signoff_signed_off = false;
    manifest.hierarchical_multisig = FeatureActivation{true, 0};
    manifest.delegation = FeatureActivation{true, 0};
    manifest.nft = FeatureActivation{true, 0};
    manifest.cadastral = FeatureActivation{true, 0};
    return manifest;
}

bool ActivationManifest::is_standard_multisig_active(Height height) const {
    return is_active(standard_multisig, height);
}

bool ActivationManifest::is_sidechain_active(Height height) const {
    return is_active(sidechain, height);
}

bool ActivationManifest::is_internal_signoff_valid() const {
    if (!internal_signoff_signed_off) {
        return false;
    }
    if (internal_signers.size() < 2) {
        return false;
    }
    std::unordered_set<std::string> uniq;
    for (const auto& signer : internal_signers) {
        if (signer.empty()) {
            return false;
        }
        uniq.insert(signer);
    }
    return uniq.size() >= 2;
}

bool ActivationManifest::is_external_audit_gate_open() const {
    return !external_audit_required || external_audit_signed_off;
}

bool ActivationManifest::is_security_gate_open() const {
    if (is_external_audit_gate_open()) {
        return true;
    }
    return is_internal_signoff_valid();
}

bool ActivationManifest::is_hierarchical_multisig_active(Height height) const {
    return is_security_gate_open() && is_active(hierarchical_multisig, height);
}

bool ActivationManifest::is_delegation_active(Height height) const {
    return is_security_gate_open() && is_active(delegation, height);
}

bool ActivationManifest::is_nft_active(Height height) const {
    return is_security_gate_open() && is_active(nft, height);
}

bool ActivationManifest::is_cadastral_active(Height height) const {
    return is_security_gate_open() && is_active(cadastral, height);
}

bool ActivationManifest::has_risky_features_enabled() const {
    return hierarchical_multisig.enabled || delegation.enabled || nft.enabled || cadastral.enabled;
}

void ActivationManifest::validate_or_throw() const {
    if (internal_signoff_signed_off && !is_internal_signoff_valid()) {
        throw std::runtime_error(
            "Activation manifest internal sign-off is invalid: need at least two distinct non-empty signers"
        );
    }

    if (!is_security_gate_open() && has_risky_features_enabled()) {
        throw std::runtime_error(
            "Activation manifest violates security gate: risky features are enabled "
            "but neither external audit sign-off nor valid internal dual-signoff is present"
        );
    }
}

std::string ActivationManifest::to_json_string(int indent) const {
    json root = {
        {"manifest_version", manifest_version},
        {"network", network},
        {"profile", profile},
        {"external_audit", {
            {"required", external_audit_required},
            {"signed_off", external_audit_signed_off},
        }},
        {"internal_signoff", {
            {"signed_off", internal_signoff_signed_off},
            {"signers", internal_signers},
            {"audited_commit", internal_audited_commit},
            {"signed_at_utc", internal_signed_at_utc},
        }},
        {"features", {
            {"standard_multisig", feature_to_json(standard_multisig)},
            {"sidechain", feature_to_json(sidechain)},
            {"hierarchical_multisig", feature_to_json(hierarchical_multisig)},
            {"delegation", feature_to_json(delegation)},
            {"nft", feature_to_json(nft)},
            {"cadastral", feature_to_json(cadastral)},
        }},
    };
    return root.dump(indent) + "\n";
}

ActivationManifest read_activation_manifest(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        throw std::runtime_error("Activation manifest not found: " + path);
    }

    json root = json::parse(in, nullptr, true, true);

    ActivationManifest manifest = ActivationManifest::production_defaults(root.value("network", "degree"));
    manifest.manifest_version = root.value("manifest_version", 1);
    manifest.profile = root.value("profile", std::string("production"));

    if (root.contains("external_audit")) {
        const auto& audit = root.at("external_audit");
        manifest.external_audit_required = audit.value("required", true);
        manifest.external_audit_signed_off = audit.value("signed_off", false);
    }
    if (root.contains("internal_signoff")) {
        const auto& signoff = root.at("internal_signoff");
        manifest.internal_signoff_signed_off = signoff.value("signed_off", false);
        manifest.internal_audited_commit = signoff.value("audited_commit", std::string{});
        manifest.internal_signed_at_utc = signoff.value("signed_at_utc", std::string{});
        if (signoff.contains("signers") && signoff.at("signers").is_array()) {
            manifest.internal_signers.clear();
            for (const auto& s : signoff.at("signers")) {
                if (s.is_string()) {
                    manifest.internal_signers.push_back(s.get<std::string>());
                }
            }
        }
    }

    const json features = root.value("features", json::object());
    manifest.standard_multisig = parse_feature(features, "standard_multisig", true);
    manifest.sidechain = parse_feature(features, "sidechain", true);
    manifest.hierarchical_multisig = parse_feature(features, "hierarchical_multisig", false);
    manifest.delegation = parse_feature(features, "delegation", false);
    manifest.nft = parse_feature(features, "nft", false);
    manifest.cadastral = parse_feature(features, "cadastral", false);
    manifest.validate_or_throw();

    return manifest;
}

bool write_activation_manifest(const std::string& path, const ActivationManifest& manifest) {
    std::filesystem::path fs_path(path);
    if (!fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }

    out << manifest.to_json_string(2);
    return out.good();
}

ActivationManifest ensure_activation_manifest(
    const std::string& path,
    const std::string& network,
    bool create_if_missing
) {
    if (std::filesystem::exists(path)) {
        ActivationManifest manifest = read_activation_manifest(path);
        if (!manifest.network.empty() && manifest.network != network) {
            throw std::runtime_error(
                "Activation manifest network mismatch: expected " + network + ", got " + manifest.network
            );
        }
        manifest.validate_or_throw();
        return manifest;
    }

    ActivationManifest manifest = ActivationManifest::production_defaults(network);
    manifest.validate_or_throw();
    if (create_if_missing && !write_activation_manifest(path, manifest)) {
        throw std::runtime_error("Failed to write activation manifest: " + path);
    }
    return manifest;
}

} // namespace geocoin::utils
