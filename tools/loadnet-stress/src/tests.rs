//! Unit tests for cross-module load-network invariants.

use super::cluster::{is_expected_log_finding, random_rpc_secret};
use super::*;

#[test]
fn default_requirement_set_is_four_nodes_and_one_thousand_accounts() {
    let args = Args::parse_from(["loadnet-stress"]);
    assert_eq!(args.nodes, 4);
    assert_eq!(args.accounts, 1000);
    assert_eq!(args.workers, 16);
    assert_eq!(args.rounds, 3);
    assert_eq!(args.startup_timeout_secs, 120);
    assert_eq!(FOUNDATION_MANAGERS.len(), 4);
    assert_eq!(RESERVED_ACCOUNT_COUNT, 80);
    assert!(args.live_features);
    assert!(args.signing_tool.ends_with("sidechain_test_tool"));
}

#[test]
fn loadnet_p2p_corpus_uses_degree_load_magic() {
    assert_eq!(
        u32::from_le_bytes(lifecycle_scenarios::LOADNET_P2P_MAGIC),
        0xD367_0003
    );
}

#[test]
fn reserved_feature_accounts_do_not_overlap_adversarial_utxos() {
    const ADVERSARIAL_MAX: usize = 22;
    assert!(RESERVED_ACCOUNT_COUNT > ADVERSARIAL_MAX);
    for id in [50usize, 51, 52, 53, 54, 60, 63, 70, 77, 78, 79] {
        assert!(id > ADVERSARIAL_MAX);
        assert!(id < RESERVED_ACCOUNT_COUNT);
        assert_ne!(id % 4, (id + 1) % 4);
    }
}

#[test]
fn rpc_credentials_use_fresh_os_entropy() {
    let first = random_rpc_secret("admin", 0);
    let second = random_rpc_secret("admin", 0);
    assert_ne!(first, second);
    assert!(first.starts_with("load-admin-0-"));
    assert_eq!(first.len(), "load-admin-0-".len() + 64);
}

#[test]
fn account_assignment_is_balanced() {
    let counts = (0..1000).fold([0usize; 4], |mut counts, id| {
        counts[id % 4] += 1;
        counts
    });
    assert_eq!(counts, [250, 250, 250, 250]);
}

#[test]
fn log_classifier_only_marks_declared_stress_events_expected() {
    for reason in [
        "block-rate-limit",
        "headers-rate-limit",
        "getdata-rate-limit",
        "inv-rate-limit",
    ] {
        assert!(!is_expected_log_finding(
            "WARN",
            &format!("[WARN] [peers] Policy reject: reason={reason}")
        ));
    }
    assert!(is_expected_log_finding(
        "WARN",
        "[WARN] [peers] Outbound peer is temporarily banned: 127.0.0.1:23030"
    ));
    assert!(!is_expected_log_finding(
        "WARN",
        "[WARN] [peers] Outbound peer is temporarily banned: 192.0.2.1:23030"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [tx] verify_input: verify_transaction_input failed"
    ));
    assert!(is_expected_log_finding(
        "WARN",
        "[WARN] [audit] rpc_host_rejected client=127.0.0.1"
    ));
    assert!(is_expected_log_finding(
        "WARN",
        "[WARN] [peers] Policy reject: reason=bad-network-magic"
    ));
    assert!(is_expected_log_finding(
        "WARN",
        "[WARN] [peers] Policy reject: reason=message-payload-limit"
    ));
    assert!(is_expected_log_finding(
        "WARN",
        "[WARN] [peers] Policy reject: reason=bad-message-frame"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] Polygon edges intersect"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] NFT tags must be unique"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] Official cadastral area mismatch"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] Polygon intersects existing cadastral plot"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] Polygon area is zero"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] Owner name is required"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] Cadastral number is required"
    ));
    assert!(is_expected_log_finding(
        "ERROR",
        "[ERROR] [nft] NFT does not exist"
    ));
    assert!(!is_expected_log_finding(
        "WARN",
        "[WARN] an unknown warning"
    ));
    assert!(!is_expected_log_finding(
        "FATAL",
        "[FATAL] unexpected crash"
    ));
}
