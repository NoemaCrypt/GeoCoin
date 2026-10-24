#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/utils/config.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    namespace fs = std::filesystem;
    using namespace geocoin::utils;

    const fs::path temp_dir = fs::temp_directory_path() / "geocoin-config-activation-test";

    try {
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        const fs::path config_path = temp_dir / "geocoin.conf";
        {
            std::ofstream out(config_path);
            out << "# test config\n";
            out << "addnode=1.2.3.4:3030\n";
            out << "addnode=5.6.7.8:3030\n";
            out << "no-auto-connect=1\n";
            out << "archive\n";
            out << "activation-manifest=custom-activation.json\n";
        }

        const auto config = read_config_file(config_path.string());
        expect(config.count("addnode") == 1, "missing addnode entries");
        expect(config.at("addnode").size() == 2, "unexpected addnode count");
        expect(config.at("no-auto-connect").size() == 1, "missing no-auto-connect");
        expect(config.at("archive").front() == "1", "flag-only config entry defaults to one");
        expect(config.at("activation-manifest").front() == "custom-activation.json", "activation-manifest mismatch");
        const auto missing_config = read_config_file((temp_dir / "missing.conf").string());
        expect(missing_config.empty(), "missing config file returns empty map");

        ActivationManifest test_manifest = ActivationManifest::fully_enabled_for_tests("degree");
        const fs::path manifest_path = temp_dir / "custom-activation.json";
        expect(write_activation_manifest(manifest_path.string(), test_manifest), "failed to write manifest");

        const ActivationManifest loaded = read_activation_manifest(manifest_path.string());
        expect(loaded.network == "degree", "loaded manifest network mismatch");
        expect(loaded.is_delegation_active(0), "delegation should be enabled in test manifest");
        expect(loaded.is_nft_active(0), "nft should be enabled in test manifest");
        expect(loaded.external_audit_signed_off, "test manifest should mark audit signed off");

        const fs::path generated_path = temp_dir / "generated-production.json";
        const ActivationManifest generated = ensure_activation_manifest(generated_path.string(), "degree-test", true);
        expect(fs::exists(generated_path), "ensure_activation_manifest did not create file");
        expect(generated.network == "degree-test", "generated manifest network mismatch");
        expect(!generated.is_delegation_active(0), "production defaults must keep delegation disabled");
        expect(generated.is_standard_multisig_active(0), "standard multisig should remain enabled");
        expect(generated.is_sidechain_active(0), "sidechain should remain enabled by default");
        expect(generated.to_json_string().find("\"sidechain\"") != std::string::npos,
               "manifest json should serialize the sidechain feature");
        expect(loaded.is_sidechain_active(0), "test manifest should keep sidechain enabled");
        expect(generated.to_json_string().find("\"manifest_version\"") != std::string::npos, "manifest json missing version");

        const fs::path legacy_path = temp_dir / "legacy-no-sidechain.json";
        {
            std::ofstream out(legacy_path);
            out << R"({
              "manifest_version": 1,
              "network": "degree",
              "profile": "production",
              "external_audit": {"required": true, "signed_off": false},
              "features": {
                "standard_multisig": {"enabled": true, "activation_height": 0}
              }
            })";
        }
        const ActivationManifest legacy = read_activation_manifest(legacy_path.string());
        expect(legacy.is_sidechain_active(0), "missing sidechain feature defaults to enabled");

        const fs::path disabled_path = temp_dir / "sidechain-disabled.json";
        {
            std::ofstream out(disabled_path);
            out << R"({
              "manifest_version": 1,
              "network": "degree",
              "profile": "production",
              "external_audit": {"required": true, "signed_off": false},
              "features": {
                "standard_multisig": {"enabled": true, "activation_height": 0},
                "sidechain": {"enabled": false, "activation_height": 0}
              }
            })";
        }
        const ActivationManifest disabled = read_activation_manifest(disabled_path.string());
        expect(!disabled.is_sidechain_active(0), "sidechain can be disabled via manifest");

        ActivationManifest gated = ActivationManifest::production_defaults("degree");
        gated.delegation = FeatureActivation{true, 0};
        expect(!gated.is_external_audit_gate_open(), "audit gate should be closed by default");
        expect(!gated.is_security_gate_open(), "security gate should be closed by default");
        expect(!gated.is_delegation_active(0), "delegation must stay inactive while audit gate is closed");

        bool threw = false;
        try {
            gated.validate_or_throw();
        } catch (...) {
            threw = true;
        }
        expect(threw, "validate_or_throw must reject risky features without audit sign-off");

        ActivationManifest internal_ok = ActivationManifest::production_defaults("degree");
        internal_ok.delegation = FeatureActivation{true, 0};
        internal_ok.internal_signoff_signed_off = true;
        internal_ok.internal_signers = {"dev_alice", "dev_bob"};
        internal_ok.internal_audited_commit = "deadbeef";
        internal_ok.internal_signed_at_utc = "2026-03-31T10:00:00Z";
        expect(internal_ok.is_internal_signoff_valid(), "internal dual-signoff should be valid");
        expect(internal_ok.is_security_gate_open(), "security gate should open with valid internal dual-signoff");
        expect(internal_ok.is_delegation_active(0), "delegation should be active with valid internal dual-signoff");
        internal_ok.validate_or_throw();

        fs::remove_all(temp_dir);
        std::cout << "test_config_and_activation: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_config_and_activation failed: " << e.what() << "\n";
        fs::remove_all(temp_dir);
        return 1;
    }
}
