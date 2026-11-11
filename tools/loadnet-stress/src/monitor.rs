//! Continuous health sampling for a running load-network cluster.

use super::*;

pub(super) fn start_monitor(
    nodes: Arc<Vec<NodeRpc>>,
    expected_outages: Arc<Vec<AtomicBool>>,
    stop: Arc<AtomicBool>,
) -> thread::JoinHandle<Vec<NetworkSample>> {
    thread::spawn(move || {
        let started = Instant::now();
        let mut samples = Vec::new();
        while !stop.load(Ordering::Acquire) {
            let mut node_samples = Vec::with_capacity(nodes.len());
            for node in nodes.iter() {
                let request_started = Instant::now();
                let expected_outage = expected_outages[node.index].load(Ordering::Acquire);
                let sample = (|| -> Result<NodeSample> {
                    let chain = node.admin.call("getblockchaininfo", json!([]))?;
                    let mempool = node.miner.call("getmempoolinfo", json!([]))?;
                    let peers: u64 = node.admin.call_as("getconnectioncount", json!([]))?;
                    let health = node.admin.call("gethealth", json!([]))?;
                    Ok(NodeSample {
                        node: node.index,
                        expected_outage,
                        height: chain.get("blocks").and_then(Value::as_u64),
                        tip: chain
                            .get("bestblockhash")
                            .and_then(Value::as_str)
                            .map(str::to_owned),
                        mempool_txs: mempool.get("size").and_then(Value::as_u64),
                        peers: Some(peers),
                        healthy: health.get("ok").and_then(Value::as_bool),
                        rpc_latency_ms: request_started.elapsed().as_millis(),
                        error: None,
                    })
                })()
                .unwrap_or_else(|error| NodeSample {
                    node: node.index,
                    expected_outage: expected_outage
                        || expected_outages[node.index].load(Ordering::Acquire),
                    height: None,
                    tip: None,
                    mempool_txs: None,
                    peers: None,
                    healthy: None,
                    rpc_latency_ms: request_started.elapsed().as_millis(),
                    error: Some(format!("{error:#}")),
                });
                node_samples.push(sample);
            }
            samples.push(NetworkSample {
                elapsed_ms: started.elapsed().as_millis(),
                nodes: node_samples,
            });
            for _ in 0..5 {
                if stop.load(Ordering::Acquire) {
                    break;
                }
                thread::sleep(Duration::from_millis(100));
            }
        }
        samples
    })
}
