use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScenarioResult {
    pub name: String,
    pub passed: bool,
    pub details: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkSample {
    pub elapsed_ms: u128,
    pub nodes: Vec<NodeSample>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeSample {
    pub node: usize,
    /// True only while the harness is deliberately stopping or starting this node.
    /// This lets the final verdict distinguish planned chaos from an unrelated outage.
    pub expected_outage: bool,
    pub height: Option<u64>,
    pub tip: Option<String>,
    pub mempool_txs: Option<u64>,
    pub peers: Option<u64>,
    pub healthy: Option<bool>,
    pub rpc_latency_ms: u128,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LogFinding {
    pub node: usize,
    pub line: usize,
    pub level: String,
    pub expected: bool,
    pub message: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TxStats {
    pub attempted: u64,
    pub accepted: u64,
    pub confirmed: u64,
    pub rejected_expected: u64,
    pub failed_unexpected: u64,
    pub broadcast_copies: u64,
    pub latency_ms: Vec<u64>,
    pub errors: BTreeMap<String, u64>,
}

impl TxStats {
    pub fn merge(&mut self, other: TxStats) {
        self.attempted += other.attempted;
        self.accepted += other.accepted;
        self.confirmed += other.confirmed;
        self.rejected_expected += other.rejected_expected;
        self.failed_unexpected += other.failed_unexpected;
        self.broadcast_copies += other.broadcast_copies;
        self.latency_ms.extend(other.latency_ms);
        for (error, count) in other.errors {
            *self.errors.entry(error).or_default() += count;
        }
    }

    pub fn percentile_ms(&self, quantile: f64) -> u64 {
        percentile(&self.latency_ms, quantile)
    }
}

#[derive(Debug, Serialize, Deserialize)]
pub struct RunReport {
    pub verdict: String,
    pub network: String,
    pub genesis_hash: String,
    pub foundation_multisig: String,
    pub foundation_managers: Vec<String>,
    pub node_count: usize,
    pub account_count: usize,
    pub blocks_mined: u64,
    pub p2p_repairs: u64,
    pub tx: TxStats,
    pub latency_p50_ms: u64,
    pub latency_p95_ms: u64,
    pub latency_p99_ms: u64,
    pub scenarios: Vec<ScenarioResult>,
    pub coverage: BTreeMap<String, bool>,
    pub samples: Vec<NetworkSample>,
    pub final_nodes: Vec<Value>,
    pub log_findings: Vec<LogFinding>,
    pub warnings: Vec<String>,
    pub fatal_errors: Vec<String>,
}

pub fn percentile(values: &[u64], percentile: f64) -> u64 {
    if values.is_empty() {
        return 0;
    }
    let mut sorted = values.to_vec();
    sorted.sort_unstable();
    let rank = ((sorted.len() - 1) as f64 * percentile.clamp(0.0, 1.0)).round() as usize;
    sorted[rank]
}

#[cfg(test)]
mod tests {
    use super::percentile;

    #[test]
    fn percentile_handles_empty_and_unsorted_values() {
        assert_eq!(percentile(&[], 0.95), 0);
        assert_eq!(percentile(&[30, 10, 20], 0.0), 10);
        assert_eq!(percentile(&[30, 10, 20], 0.5), 20);
        assert_eq!(percentile(&[30, 10, 20], 1.0), 30);
    }
}
