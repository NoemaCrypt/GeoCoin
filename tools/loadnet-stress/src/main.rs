mod cluster;
mod feature_scenarios;
mod lifecycle_scenarios;
mod monitor;
mod report;
mod rpc;
mod rpc_scenarios;
#[cfg(test)]
mod tests;
mod transaction_scenarios;
mod transactions;

use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, OpenOptions};
use std::io::{ErrorKind, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail, ensure};
use clap::Parser;
use cluster::{Cluster, NodeRpc};
use feature_scenarios::run_maximal_feature_scenarios;
use lifecycle_scenarios::{
    run_crash_restart_scenario, run_live_p2p_fail_closed_scenario,
    run_reorg_and_mempool_resurrection_scenario,
};
use monitor::start_monitor;
use rand::RngCore;
use rand::rngs::{OsRng, StdRng};
use rand::seq::SliceRandom;
use rand::{Rng, SeedableRng};
use report::{LogFinding, NetworkSample, NodeSample, RunReport, ScenarioResult, TxStats};
use rpc::{RpcClient, RpcError, basic_auth_base64};
use rpc_scenarios::run_live_rpc_security_scenarios;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use transaction_scenarios::{
    run_confirmed_replay_scenario, run_negative_and_boundary_scenarios,
    run_transaction_limit_scenarios,
};
use transactions::*;

const NETWORK: &str = "degree-load";
const GENESIS_HASH: &str = "63569701efc8d950c0ee1bfca30d692dd1ff00647f5c48c515add8a615f70675";
const FOUNDATION_MULTISIG: &str = "GEOM3EuJq9xB1xXoUjDJ6MK6DR28HhxYseseC2";
const FOUNDATION_MANAGERS: [&str; 4] = [
    "GEO1P3rsV9e3HpE8yfR8VPtjNqwrntcNH3ZKk",
    "GEO1K9Juep1tJoUGkSdhmGiPhvgqc5WFoyAo8",
    "GEO1LiZwqv6oqA297U6CN76erHUpDJQFcBja7",
    "GEO1N4hgEstUmnPUBh7VJs9vdEebnVokALf3R",
];
const COINBASE_MATURITY: u64 = 100;
const DUST_ANGLES: u64 = 1_000;
const MAX_TX_SIZE: usize = 100_000;
const MAX_TX_INPUTS: usize = 1_000;
const MAX_TX_OUTPUTS: usize = 1_000;
const MAX_TESTMEMPOOLACCEPT_BATCH: usize = 25;
const RPC_BATCH_LIMIT: usize = 10;
const SECRET_LOG_CANARY: &str = "loadnet-secret-canary-must-never-appear";
const RESERVED_ACCOUNT_COUNT: usize = 80;

#[derive(Debug, Parser)]
#[command(name = "geocoin-loadnet-stress")]
#[command(about = "Build, fund, load and continuously analyze an isolated GeoCoin cluster")]
struct Args {
    #[arg(long, default_value = "./cmake-build-debug/geocoin-node")]
    node_binary: PathBuf,

    #[arg(long, default_value = "./cmake-build-debug/geocoin-rpc-miner")]
    miner_binary: PathBuf,

    #[arg(
        long,
        default_value = "./config/activation-manifest.testing-loadnet.json"
    )]
    activation_manifest: PathBuf,

    /// Must be absent or empty; existing node data is never overwritten.
    #[arg(long, default_value = "./loadnet-runs/manual")]
    work_dir: PathBuf,

    #[arg(long, default_value_t = 4)]
    nodes: usize,

    #[arg(long, default_value_t = 1000)]
    accounts: usize,

    #[arg(long, default_value_t = 16)]
    workers: usize,

    #[arg(long, default_value_t = 3)]
    rounds: usize,

    /// Zero means every account sends one transaction in each round.
    #[arg(long, default_value_t = 0)]
    txs_per_round: usize,

    #[arg(long, default_value_t = 10_000_000)]
    funding_angles: u64,

    #[arg(long, default_value_t = 100_000)]
    fee_angles: u64,

    #[arg(long, default_value_t = 1_000_000)]
    fanout_fee_angles: u64,

    #[arg(long, default_value_t = 10_000)]
    min_transfer_angles: u64,

    #[arg(long, default_value_t = 500_000)]
    max_transfer_angles: u64,

    /// Covers cold RocksDB WAL recovery after an intentional SIGKILL.
    #[arg(long, default_value_t = 120)]
    startup_timeout_secs: u64,

    #[arg(long, default_value_t = 90)]
    mining_timeout_secs: u64,

    #[arg(long, default_value_t = true, action = clap::ArgAction::Set)]
    restart_follower: bool,

    /// Exercise a real competing-chain reorganization and mempool resurrection.
    #[arg(long, default_value_t = true, action = clap::ArgAction::Set)]
    reorg: bool,

    /// Crash (SIGKILL-equivalent) and recover one follower after durable writes.
    #[arg(long, default_value_t = true, action = clap::ArgAction::Set)]
    crash_restart: bool,

    /// Send a malformed pre-authentication P2P frame and require fail-closed handling.
    #[arg(long, default_value_t = true, action = clap::ArgAction::Set)]
    adversarial_p2p: bool,

    #[arg(long, default_value_t = 5_000)]
    max_tx_p99_ms: u64,

    #[arg(long, default_value_t = 10_000)]
    max_monitor_rpc_latency_ms: u64,

    /// Leave node processes running after the report is written.
    #[arg(long, default_value_t = false)]
    keep_nodes: bool,

    #[arg(long, default_value_t = 0xD367_2026)]
    seed: u64,

    /// Falcon payload signer used for live sidechain owner/committee signatures.
    #[arg(long, default_value = "./cmake-build-debug/sidechain_test_tool")]
    signing_tool: PathBuf,

    /// NFT, sidechain, query-consistency and extra payment/chaos scenarios.
    #[arg(long, default_value_t = true, action = clap::ArgAction::Set)]
    live_features: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct Account {
    id: usize,
    owner_node: usize,
    address: String,
}

#[derive(Clone)]
struct TransferTask {
    account_index: usize,
    recipient: String,
    amount: u64,
}

#[derive(Clone)]
struct SignedTx {
    txid: String,
    hex: String,
}

#[derive(Debug)]
struct TransferOutcome {
    txid: Option<String>,
    raw_hex: Option<String>,
    latency_ms: u64,
    broadcast_copies: u64,
    error: Option<String>,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("[loadnet] FAILED: {error:#}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let args = Args::parse();
    validate_args(&args)?;
    prepare_work_dir(&args.work_dir)?;

    println!(
        "[loadnet] starting {} isolated nodes in {}",
        args.nodes,
        args.work_dir.display()
    );
    let mut cluster = Cluster::start(&args)?;
    cluster.keep_alive = args.keep_nodes;

    let monitor_stop = Arc::new(AtomicBool::new(false));
    let monitor_handle = start_monitor(
        cluster.rpc_nodes(),
        Arc::clone(&cluster.expected_monitor_outages),
        Arc::clone(&monitor_stop),
    );

    let mut coverage = BTreeMap::new();
    let mut scenarios = Vec::new();
    let mut tx_stats = TxStats::default();
    let mut fatal_errors = Vec::new();
    let mut warnings = Vec::new();
    let mut intentionally_restarted_nodes = BTreeSet::new();

    let workflow_result = (|| -> Result<(Vec<Account>, Vec<Value>)> {
        cluster.create_wallets()?;
        cluster.connect_full_mesh()?;
        cluster.verify_genesis_and_foundation()?;
        cluster.assert_secret_file_permissions()?;
        coverage.insert("isolated_loadnet_genesis".to_owned(), true);
        coverage.insert("foundation_4_of_4".to_owned(), true);
        coverage.insert("secret_files_private".to_owned(), true);
        run_live_rpc_security_scenarios(&cluster, &mut scenarios)?;
        coverage.insert("live_rpc_auth_and_parser_security".to_owned(), true);

        println!(
            "[loadnet] generating {} wallet-owned accounts",
            args.accounts
        );
        let accounts = cluster.generate_accounts(args.accounts)?;
        write_accounts(&args.work_dir, &accounts)?;
        coverage.insert(
            "accounts_created_and_persisted".to_owned(),
            accounts.len() == args.accounts,
        );

        // Four early coinbases are needed for four independent fan-out inputs;
        // +4 guarantees all of them have 100 confirmations.
        let bootstrap_blocks = COINBASE_MATURITY + 4;
        println!(
            "[loadnet] mining {bootstrap_blocks} fixed-difficulty bootstrap blocks with one CPU thread"
        );
        cluster.mine_blocks(bootstrap_blocks)?;
        cluster.assert_fixed_difficulty()?;
        coverage.insert("fixed_trivial_pow".to_owned(), true);
        coverage.insert("mature_coinbase".to_owned(), true);

        println!("[loadnet] funding accounts with four independent fan-out transactions");
        let funding_txids =
            cluster.fund_accounts(&accounts, args.funding_angles, args.fanout_fee_angles)?;
        cluster.mine_until_mempools_empty(12)?;
        cluster.verify_all_accounts_funded(&accounts, args.funding_angles)?;
        coverage.insert(
            "four_way_fanout_funding".to_owned(),
            funding_txids.len() == 4,
        );

        let mut rng = StdRng::seed_from_u64(args.seed);
        let negative_valid_transactions = run_negative_and_boundary_scenarios(
            &cluster,
            &accounts,
            &args,
            &mut scenarios,
            &mut tx_stats,
        )?;
        coverage.insert(
            "negative_policy_scenarios".to_owned(),
            scenarios.iter().all(|item| item.passed),
        );
        if !negative_valid_transactions.is_empty() {
            cluster.mine_until_mempools_empty(8)?;
            let txids = negative_valid_transactions
                .iter()
                .map(|tx| tx.txid.clone())
                .collect::<Vec<_>>();
            let confirmed = confirm_transactions(&cluster.nodes[0].rpc.admin, &txids)?;
            ensure!(
                confirmed == txids.len() as u64,
                "adversarial valid set confirmed {confirmed}/{} transactions",
                txids.len()
            );
            tx_stats.confirmed += confirmed;
            run_confirmed_replay_scenario(
                &cluster,
                &negative_valid_transactions,
                "confirmed_adversarial_valid_transaction_replay_rejected_by_every_node",
                &mut scenarios,
                &mut tx_stats,
            )?;
        }
        run_transaction_limit_scenarios(
            &mut cluster,
            &accounts,
            &args,
            &mut scenarios,
            &mut tx_stats,
        )?;
        coverage.insert("transaction_consensus_boundaries".to_owned(), true);

        if args.live_features {
            println!(
                "[loadnet] running maximal live NFT/sidechain/query/chaos scenarios"
            );
            run_maximal_feature_scenarios(
                &mut cluster,
                &accounts,
                &args,
                &mut scenarios,
                &mut tx_stats,
            )?;
            cluster.verify_all_accounts_funded(
                &accounts[RESERVED_ACCOUNT_COUNT..],
                args.funding_angles,
            )?;
            intentionally_restarted_nodes.insert(0);
            coverage.insert("live_nft_create_transfer_burn".to_owned(), true);
            coverage.insert("live_sidechain_unregistered_and_financial".to_owned(), true);
            coverage.insert("live_accounting_sidechain_and_redeem".to_owned(), true);
            coverage.insert("cross_node_query_consistency".to_owned(), true);
            coverage.insert("primary_miner_restart".to_owned(), true);
            coverage.insert(
                "load_accounts_untouched_by_live_features".to_owned(),
                true,
            );
        }

        let transferable: Vec<usize> = (RESERVED_ACCOUNT_COUNT..accounts.len()).collect();
        let txs_per_round = if args.txs_per_round == 0 {
            transferable.len()
        } else {
            args.txs_per_round.min(transferable.len())
        };

        for round in 0..args.rounds {
            let mut indices = transferable.clone();
            indices.shuffle(&mut rng);
            indices.truncate(txs_per_round);
            let tasks = indices
                .into_iter()
                .map(|account_index| {
                    let mut recipient_index = if round % 3 == 1 {
                        RESERVED_ACCOUNT_COUNT
                    } else {
                        rng.gen_range(RESERVED_ACCOUNT_COUNT..accounts.len())
                    };
                    if recipient_index == account_index {
                        recipient_index = RESERVED_ACCOUNT_COUNT
                            + ((recipient_index - RESERVED_ACCOUNT_COUNT + 1)
                                % (accounts.len() - RESERVED_ACCOUNT_COUNT));
                    }
                    TransferTask {
                        account_index,
                        recipient: accounts[recipient_index].address.clone(),
                        amount: rng.gen_range(args.min_transfer_angles..=args.max_transfer_angles),
                    }
                })
                .collect::<Vec<_>>();

            println!(
                "[loadnet] round {}/{}: {} transfers on {} worker threads",
                round + 1,
                args.rounds,
                tasks.len(),
                args.workers,
            );
            let (round_stats, txids) = run_transfer_batch(
                cluster.rpc_nodes(),
                Arc::new(accounts.clone()),
                Arc::new(tasks),
                args.workers,
                args.fee_angles,
            );
            let round_failures = round_stats.failed_unexpected;
            let round_errors = round_stats.errors.clone();
            tx_stats.merge(round_stats);
            ensure!(
                round_failures == 0,
                "round {} had {} unexpected transaction failures: {:?}",
                round + 1,
                round_failures,
                round_errors
            );
            cluster.mine_until_mempools_empty(20)?;
            let confirmed = confirm_transactions(&cluster.nodes[0].rpc.admin, &txids)?;
            ensure!(
                confirmed == txids.len() as u64,
                "round {} confirmed {confirmed}/{} transactions",
                round + 1,
                txids.len()
            );
            tx_stats.confirmed += confirmed;

            if args.restart_follower && round == 0 && cluster.nodes.len() > 2 {
                let follower = cluster.nodes.len() - 1;
                println!(
                    "[loadnet] restarting follower node {follower} and checking persistence/resync"
                );
                cluster.restart_node(follower)?;
                intentionally_restarted_nodes.insert(follower);
                cluster.connect_full_mesh()?;
                cluster.ensure_converged(cluster.height(0)?)?;
                let owned = accounts
                    .iter()
                    .find(|account| account.owner_node == follower)
                    .ok_or_else(|| anyhow!("restarted follower owns no test account"))?;
                let utxos = cluster.nodes[follower]
                    .rpc
                    .admin
                    .call("getaddressutxos", json!([owned.address]))?;
                let passed = utxos.as_array().is_some_and(|items| !items.is_empty());
                record_scenario(
                    &mut scenarios,
                    "follower_restart_wallet_chain_persistence",
                    passed,
                    format!(
                        "node {follower} recovered account {} and converged",
                        owned.address
                    ),
                )?;
                coverage.insert("restart_and_resync".to_owned(), true);
            }
        }

        coverage.insert("randomized_and_hotspot_transfer_patterns".to_owned(), true);
        if args.crash_restart {
            let restarted = run_crash_restart_scenario(&mut cluster, &accounts, &mut scenarios)?;
            intentionally_restarted_nodes.insert(restarted);
            coverage.insert("unclean_crash_recovery".to_owned(), true);
        }
        if args.reorg {
            let restarted = run_reorg_and_mempool_resurrection_scenario(
                &mut cluster,
                &accounts,
                &args,
                &mut scenarios,
                &mut tx_stats,
            )?;
            intentionally_restarted_nodes.extend(restarted);
            coverage.insert("real_competing_chain_reorg".to_owned(), true);
            coverage.insert("reorg_mempool_resurrection".to_owned(), true);
        }

        coverage.insert(
            "multithreaded_account_transfers".to_owned(),
            tx_stats.accepted > 0 && tx_stats.failed_unexpected == 0,
        );
        cluster.ensure_converged(cluster.height(0)?)?;
        cluster.assert_all_mempools_empty()?;
        cluster.connect_full_mesh()?;
        coverage.insert("p2p_full_mesh".to_owned(), true);
        coverage.insert("p2p_tip_convergence".to_owned(), true);
        if args.adversarial_p2p {
            run_live_p2p_fail_closed_scenario(&cluster, &mut scenarios)?;
            coverage.insert("live_p2p_malformed_input_fail_closed".to_owned(), true);
        }
        cluster.assert_processes_alive()?;
        coverage.insert("all_node_processes_alive".to_owned(), true);
        let secrets_absent = !cluster.logs_contain(SECRET_LOG_CANARY)?;
        record_scenario(
            &mut scenarios,
            "rpc_credentials_and_parse_secrets_absent_from_logs",
            secrets_absent,
            "searched every node log for the live secret canary",
        )?;
        coverage.insert("secret_log_redaction".to_owned(), secrets_absent);
        let final_nodes = cluster.collect_final_diagnostics()?;
        Ok((accounts, final_nodes))
    })();

    monitor_stop.store(true, Ordering::Release);
    let samples = monitor_handle.join().unwrap_or_else(|_| Vec::new());

    let (accounts, final_nodes) = match workflow_result {
        Ok(result) => result,
        Err(error) => {
            fatal_errors.push(format!("{error:#}"));
            (
                read_accounts(&args.work_dir).unwrap_or_default(),
                cluster.collect_final_diagnostics().unwrap_or_default(),
            )
        }
    };

    let monitoring_has_data = !samples.is_empty()
        && samples
            .iter()
            .any(|sample| sample.nodes.iter().any(|node| node.height.is_some()));
    let monitor_failures = samples
        .iter()
        .flat_map(|sample| sample.nodes.iter())
        .filter(|node| node.error.is_some() || node.healthy == Some(false))
        .collect::<Vec<_>>();
    let expected_monitor_failures = monitor_failures
        .iter()
        .filter(|sample| sample.expected_outage)
        .count();
    let unexpected_monitor_failures = monitor_failures
        .iter()
        .filter(|sample| !sample.expected_outage)
        .count();
    if expected_monitor_failures > 0 {
        warnings.push(format!(
            "monitor observed {expected_monitor_failures} failures inside explicitly marked restart/partition windows for nodes {:?}; final recovery checks passed",
            intentionally_restarted_nodes,
        ));
    }
    coverage.insert(
        "continuous_network_analysis".to_owned(),
        monitoring_has_data && unexpected_monitor_failures == 0,
    );
    let max_monitor_latency = samples
        .iter()
        .flat_map(|sample| sample.nodes.iter())
        .filter(|node| node.error.is_none())
        .map(|node| node.rpc_latency_ms)
        .max()
        .unwrap_or(0);
    coverage.insert(
        "monitor_rpc_latency_within_budget".to_owned(),
        max_monitor_latency <= u128::from(args.max_monitor_rpc_latency_ms),
    );

    let log_findings = match cluster.scan_logs() {
        Ok(findings) => findings,
        Err(error) => {
            fatal_errors.push(format!("cannot analyze node logs: {error:#}"));
            Vec::new()
        }
    };
    let unexpected_log_errors = log_findings
        .iter()
        .filter(|finding| matches!(finding.level.as_str(), "ERROR" | "FATAL") && !finding.expected)
        .count();
    let warning_count = log_findings
        .iter()
        .filter(|finding| finding.level == "WARN")
        .count();
    let unexpected_warning_count = log_findings
        .iter()
        .filter(|finding| finding.level == "WARN" && !finding.expected)
        .count();
    let expected_error_count = log_findings
        .iter()
        .filter(|finding| finding.level == "ERROR" && finding.expected)
        .count();
    coverage.insert(
        "node_logs_no_unexpected_findings".to_owned(),
        unexpected_log_errors == 0 && unexpected_warning_count == 0,
    );
    if warning_count > 0 || expected_error_count > 0 {
        warnings.push(format!(
            "node logs contain {warning_count} warnings and {expected_error_count} expected errors from negative/restart scenarios; see log_findings"
        ));
    }
    if unexpected_warning_count > 0 {
        warnings.push(format!(
            "node logs contain {unexpected_warning_count} unclassified warnings; see log_findings entries with expected=false"
        ));
    }
    if cluster.p2p_repairs > 0 {
        warnings.push(format!(
            "{} mined batches needed diagnostic direct block repair; convergence was recovered, but the run is failed because normal P2P relay did not converge on its own",
            cluster.p2p_repairs
        ));
    }
    coverage.insert(
        "p2p_relay_without_direct_repairs".to_owned(),
        cluster.p2p_repairs == 0,
    );

    coverage.insert(
        "transaction_p99_latency_within_budget".to_owned(),
        tx_stats.percentile_ms(0.99) <= args.max_tx_p99_ms,
    );

    let all_coverage = !coverage.is_empty() && coverage.values().all(|passed| *passed);
    let all_scenarios = scenarios.iter().all(|scenario| scenario.passed);
    let all_accepted_confirmed = tx_stats.accepted == tx_stats.confirmed;
    let passed = fatal_errors.is_empty()
        && all_coverage
        && all_scenarios
        && tx_stats.failed_unexpected == 0
        && all_accepted_confirmed;

    let report = RunReport {
        verdict: if passed { "PASS" } else { "FAIL" }.to_owned(),
        network: NETWORK.to_owned(),
        genesis_hash: GENESIS_HASH.to_owned(),
        foundation_multisig: FOUNDATION_MULTISIG.to_owned(),
        foundation_managers: FOUNDATION_MANAGERS
            .iter()
            .map(|value| (*value).to_owned())
            .collect(),
        node_count: cluster.nodes.len(),
        account_count: accounts.len(),
        blocks_mined: cluster.blocks_mined,
        p2p_repairs: cluster.p2p_repairs,
        latency_p50_ms: tx_stats.percentile_ms(0.50),
        latency_p95_ms: tx_stats.percentile_ms(0.95),
        latency_p99_ms: tx_stats.percentile_ms(0.99),
        tx: tx_stats,
        scenarios,
        coverage,
        samples,
        final_nodes,
        log_findings,
        warnings,
        fatal_errors,
    };

    let report_path = args.work_dir.join("report.json");
    fs::write(&report_path, serde_json::to_vec_pretty(&report)?)?;
    println!(
        "[loadnet] verdict={} accounts={} accepted={} confirmed={} blocks={} p2p_repairs={}",
        report.verdict,
        report.account_count,
        report.tx.accepted,
        report.tx.confirmed,
        report.blocks_mined,
        report.p2p_repairs,
    );
    println!("[loadnet] report: {}", report_path.display());

    if !passed {
        bail!(
            "loadnet report verdict is FAIL (see {})",
            report_path.display()
        );
    }
    Ok(())
}

fn validate_args(args: &Args) -> Result<()> {
    ensure!(
        args.node_binary.is_file(),
        "node binary not found: {}",
        args.node_binary.display()
    );
    ensure!(
        args.miner_binary.is_file(),
        "miner binary not found: {}",
        args.miner_binary.display()
    );
    ensure!(
        args.activation_manifest.is_file(),
        "activation manifest not found: {}",
        args.activation_manifest.display()
    );
    ensure!((2..=8).contains(&args.nodes), "--nodes must be in 2..=8");
    ensure!(
        args.accounts >= 128,
        "--accounts must be at least 128 so reserved adversarial/feature UTXOs remain independent of load rounds"
    );
    ensure!(
        (1..=64).contains(&args.workers),
        "--workers must be in 1..=64"
    );
    ensure!(args.rounds >= 1, "--rounds must be positive");
    ensure!(
        args.startup_timeout_secs > 0,
        "--startup-timeout-secs must be positive"
    );
    ensure!(args.max_tx_p99_ms > 0, "--max-tx-p99-ms must be positive");
    ensure!(
        args.max_monitor_rpc_latency_ms > 0,
        "--max-monitor-rpc-latency-ms must be positive"
    );
    ensure!(
        args.funding_angles > args.max_transfer_angles + args.fee_angles + DUST_ANGLES,
        "funding must cover the largest transfer, fee and non-dust change"
    );
    ensure!(
        args.min_transfer_angles >= DUST_ANGLES
            && args.min_transfer_angles <= args.max_transfer_angles,
        "invalid transfer amount range"
    );
    if args.live_features {
        ensure!(
            args.signing_tool.is_file(),
            "sidechain signing tool not found: {}",
            args.signing_tool.display()
        );
    }
    Ok(())
}

fn prepare_work_dir(path: &Path) -> Result<()> {
    if path.exists() {
        let mut entries =
            fs::read_dir(path).with_context(|| format!("cannot inspect {}", path.display()))?;
        ensure!(
            entries.next().is_none(),
            "work directory already exists and is not empty: {}",
            path.display()
        );
    } else {
        fs::create_dir_all(path)?;
    }
    Ok(())
}
