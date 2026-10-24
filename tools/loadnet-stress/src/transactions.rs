//! Transaction construction, submission, and assertion helpers.

use super::*;
use std::collections::HashSet;

pub(super) fn write_accounts(root: &Path, accounts: &[Account]) -> Result<()> {
    fs::write(
        root.join("accounts.json"),
        serde_json::to_vec_pretty(accounts)?,
    )?;
    let mut text = String::new();
    for account in accounts {
        text.push_str(&format!(
            "{:04}\tnode-{}\t{}\n",
            account.id, account.owner_node, account.address
        ));
    }
    fs::write(root.join("accounts.txt"), text)?;
    Ok(())
}

pub(super) fn read_accounts(root: &Path) -> Result<Vec<Account>> {
    let path = root.join("accounts.json");
    let bytes = fs::read(&path)
        .with_context(|| format!("cannot read persisted accounts from {}", path.display()))?;
    serde_json::from_slice(&bytes)
        .with_context(|| format!("cannot parse persisted accounts from {}", path.display()))
}

pub(super) fn sign_raw(client: &RpcClient, raw: &str) -> Result<SignedTx> {
    let signed = client.call("signrawtransactionwithwallet", json!([raw]))?;
    ensure!(
        signed.get("complete").and_then(Value::as_bool) == Some(true),
        "wallet could not completely sign transaction: {signed}"
    );
    Ok(SignedTx {
        txid: signed
            .get("txid")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("signed transaction lacks txid"))?
            .to_owned(),
        hex: signed
            .get("hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("signed transaction lacks hex"))?
            .to_owned(),
    })
}

pub(super) fn preflight_allowed(client: &RpcClient, raw: &str) -> Result<String> {
    let result = client.call("testmempoolaccept", json!([[raw]]))?;
    let item = result
        .as_array()
        .and_then(|items| items.first())
        .ok_or_else(|| anyhow!("testmempoolaccept returned no item"))?;
    ensure!(
        item.get("allowed").and_then(Value::as_bool) == Some(true),
        "transaction preflight rejected: {item}"
    );
    Ok(item
        .get("txid")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned())
}

pub(super) fn is_duplicate_error(error: &RpcError) -> bool {
    error.is_remote()
        && error.remote_message().is_some_and(|message| {
            let lower = message.to_lowercase();
            lower.contains("already")
                || lower.contains("duplicate")
                || (error.remote_code() == Some(-25) && lower == "mempool-rejected")
        })
}

pub(super) fn transaction_known(client: &RpcClient, txid: &str) -> bool {
    client.call("getmempoolentry", json!([txid])).is_ok()
        || client.call("getrawtransaction", json!([txid, 0])).is_ok()
}

pub(super) fn broadcast_signed(nodes: &Arc<Vec<NodeRpc>>, tx: &SignedTx) -> Result<u64> {
    let mut copies = 0;
    for node in nodes.iter() {
        match node.admin.call("sendrawtransaction", json!([tx.hex])) {
            Ok(value) => {
                ensure!(
                    value.as_str().is_none_or(|reported| reported == tx.txid),
                    "node {} returned a different txid for broadcast: {value}",
                    node.index
                );
                copies += 1;
            }
            Err(error)
                if is_duplicate_error(&error) && transaction_known(&node.admin, &tx.txid) =>
            {
                copies += 1;
            }
            Err(error) => bail!("node {} rejected broadcast: {error}", node.index),
        }
    }
    Ok(copies)
}

pub(super) fn select_utxo(client: &RpcClient, address: &str, required: u64) -> Result<Value> {
    let utxos = client.call("getaddressutxos", json!([address, 0, 5000]))?;
    utxos
        .as_array()
        .ok_or_else(|| anyhow!("getaddressutxos returned non-array"))?
        .iter()
        .filter(|utxo| {
            utxo.get("value").and_then(Value::as_u64).unwrap_or(0) > required + DUST_ANGLES
        })
        .max_by_key(|utxo| utxo.get("value").and_then(Value::as_u64).unwrap_or(0))
        .cloned()
        .ok_or_else(|| anyhow!("no UTXO for {address} covers {required} angles"))
}

pub(super) fn build_signed_transfer(
    node: &NodeRpc,
    sender: &str,
    recipient: &str,
    amount: u64,
    fee: u64,
    selected: Option<Value>,
) -> Result<SignedTx> {
    let utxo = match selected {
        Some(value) => value,
        None => select_utxo(&node.admin, sender, amount + fee)?,
    };
    let value = utxo
        .get("value")
        .and_then(Value::as_u64)
        .ok_or_else(|| anyhow!("selected UTXO has no exact value"))?;
    ensure!(
        value > amount + fee + DUST_ANGLES,
        "selected UTXO would create dust change"
    );
    let inputs = json!([{
        "txid": utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
        "vout": utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
    }]);
    let outputs = json!([
        {"address": recipient, "value_angles": amount},
        {"address": sender, "value_angles": value - amount - fee},
    ]);
    let raw: String = node
        .admin
        .call_as("createrawtransaction", json!([inputs, outputs]))?;
    sign_raw(&node.admin, &raw)
}

pub(super) fn mempool_invariant(client: &RpcClient) -> Result<(u64, u64, u64, u64)> {
    let info = client.call("getmempoolinfo", json!([]))?;
    Ok((
        info.get("size").and_then(Value::as_u64).unwrap_or(0),
        info.get("bytes").and_then(Value::as_u64).unwrap_or(0),
        info.get("transactions_updated")
            .and_then(Value::as_u64)
            .unwrap_or(0),
        info.get("total_fee_angles")
            .and_then(Value::as_u64)
            .unwrap_or(0),
    ))
}

pub(super) fn preflight_item(client: &RpcClient, raw: &str) -> Result<Value> {
    client
        .call("testmempoolaccept", json!([[raw]]))?
        .as_array()
        .and_then(|items| items.first())
        .cloned()
        .ok_or_else(|| anyhow!("testmempoolaccept returned no item"))
}

pub(super) fn preflight_rejected(client: &RpcClient, raw: &str) -> Result<(bool, String)> {
    match preflight_item(client, raw) {
        Ok(item) => Ok((
            item.get("allowed").and_then(Value::as_bool) == Some(false),
            item.to_string(),
        )),
        Err(error) => Ok((
            false,
            format!("preflight transport/protocol error: {error:#}"),
        )),
    }
}

pub(super) fn wait_for_mempool_relay(
    nodes: &Arc<Vec<NodeRpc>>,
    txid: &str,
    raw: &str,
    timeout: Duration,
) -> Result<()> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if nodes
            .iter()
            .all(|node| node.admin.call("getmempoolentry", json!([txid])).is_ok())
        {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(100));
    }
    let diagnostics = nodes
        .iter()
        .map(|node| {
            let present = node.admin.call("getmempoolentry", json!([txid])).is_ok();
            let preflight = if present {
                "already-present".to_owned()
            } else {
                preflight_item(&node.admin, raw)
                    .map(|value| value.to_string())
                    .unwrap_or_else(|error| format!("diagnostic-error: {error:#}"))
            };
            let authenticated_peers = node
                .admin
                .call("getconnectiondiagnostics", json!([]))
                .ok()
                .and_then(|value| {
                    value
                        .get("stats")
                        .and_then(|stats| stats.get("authenticated_peers"))
                        .and_then(Value::as_u64)
                });
            (node.index, present, authenticated_peers, preflight)
        })
        .collect::<Vec<_>>();
    bail!("transaction {txid} did not relay to every mempool: {diagnostics:?}")
}

pub(super) fn submit_single_origin_and_wait(
    nodes: &Arc<Vec<NodeRpc>>,
    origin: usize,
    tx: &SignedTx,
    timeout: Duration,
) -> Result<()> {
    let reported = nodes[origin]
        .admin
        .call("sendrawtransaction", json!([tx.hex]))?;
    ensure!(
        reported.as_str() == Some(tx.txid.as_str()),
        "origin node returned unexpected txid: {reported}"
    );
    wait_for_mempool_relay(nodes, &tx.txid, &tx.hex, timeout)
}

pub(super) fn sign_raw_result(client: &RpcClient, raw: &str) -> Result<Value> {
    Ok(client.call("signrawtransactionwithwallet", json!([raw]))?)
}

pub(super) fn run_transfer_batch(
    nodes: Arc<Vec<NodeRpc>>,
    accounts: Arc<Vec<Account>>,
    tasks: Arc<Vec<TransferTask>>,
    workers: usize,
    fee: u64,
) -> (TxStats, Vec<String>) {
    let next = Arc::new(AtomicUsize::new(0));
    let outcomes = Arc::new(Mutex::new(Vec::with_capacity(tasks.len())));
    let mut handles = Vec::new();
    for _ in 0..workers {
        let nodes = Arc::clone(&nodes);
        let accounts = Arc::clone(&accounts);
        let tasks = Arc::clone(&tasks);
        let next = Arc::clone(&next);
        let outcomes = Arc::clone(&outcomes);
        handles.push(thread::spawn(move || {
            loop {
                let index = next.fetch_add(1, Ordering::Relaxed);
                if index >= tasks.len() {
                    break;
                }
                let task = &tasks[index];
                let account = &accounts[task.account_index];
                let started = Instant::now();
                let result = (|| -> Result<(SignedTx, u64)> {
                    let signed = build_signed_transfer(
                        &nodes[account.owner_node],
                        &account.address,
                        &task.recipient,
                        task.amount,
                        fee,
                        None,
                    )?;
                    let preflight_txid =
                        preflight_allowed(&nodes[account.owner_node].admin, &signed.hex)?;
                    ensure!(
                        preflight_txid.is_empty() || preflight_txid == signed.txid,
                        "preflight/signing txid mismatch"
                    );
                    let copies = broadcast_signed(&nodes, &signed)?;
                    Ok((signed, copies))
                })();
                let latency_ms = started.elapsed().as_millis().min(u64::MAX as u128) as u64;
                let outcome = match result {
                    Ok((signed, copies)) => TransferOutcome {
                        txid: Some(signed.txid),
                        raw_hex: Some(signed.hex),
                        latency_ms,
                        broadcast_copies: copies,
                        error: None,
                    },
                    Err(error) => TransferOutcome {
                        txid: None,
                        raw_hex: None,
                        latency_ms,
                        broadcast_copies: 0,
                        error: Some(format!("{error:#}")),
                    },
                };
                outcomes
                    .lock()
                    .expect("outcome mutex poisoned")
                    .push(outcome);
            }
        }));
    }
    for handle in handles {
        let _ = handle.join();
    }

    let outcomes = outcomes.lock().expect("outcome mutex poisoned");
    let mut stats = TxStats {
        attempted: tasks.len() as u64,
        ..TxStats::default()
    };
    let mut txids = Vec::new();
    for outcome in outcomes.iter() {
        stats.latency_ms.push(outcome.latency_ms);
        stats.broadcast_copies += outcome.broadcast_copies;
        if let Some(txid) = &outcome.txid {
            stats.accepted += 1;
            txids.push(txid.clone());
        } else {
            stats.failed_unexpected += 1;
            let error = outcome
                .error
                .clone()
                .unwrap_or_else(|| "unknown".to_owned());
            *stats.errors.entry(error).or_default() += 1;
        }
        let _ = &outcome.raw_hex;
    }
    (stats, txids)
}

pub(super) fn confirm_transactions(client: &RpcClient, txids: &[String]) -> Result<u64> {
    if txids.is_empty() {
        return Ok(0);
    }
    let tip = client.call_as::<u64>("getblockcount", json!([]))?;
    let mut in_chain = HashSet::new();
    for height in 0..=tip {
        let block = client.call("getblockbyheight", json!([height]))?;
        let Some(block_txids) = block.get("tx").and_then(Value::as_array) else {
            continue;
        };
        for txid in block_txids {
            if let Some(txid) = txid.as_str() {
                in_chain.insert(txid.to_owned());
            }
        }
    }
    let mempool = client.call("getrawmempool", json!([]))?;
    let in_mempool = mempool
        .as_array()
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .collect::<HashSet<_>>()
        })
        .unwrap_or_default();
    let mut confirmed = 0;
    for txid in txids {
        if in_chain.contains(txid) && !in_mempool.contains(txid.as_str()) {
            confirmed += 1;
        }
    }
    Ok(confirmed)
}

pub(super) fn record_scenario(
    scenarios: &mut Vec<ScenarioResult>,
    name: &str,
    passed: bool,
    details: impl Into<String>,
) -> Result<()> {
    scenarios.push(ScenarioResult {
        name: name.to_owned(),
        passed,
        details: details.into(),
    });
    ensure!(passed, "scenario {name} failed");
    Ok(())
}

pub(super) fn expected_rejection(
    scenarios: &mut Vec<ScenarioResult>,
    name: &str,
    result: Result<Value, RpcError>,
) -> Result<()> {
    let (passed, details) = match result {
        Ok(value) => (false, format!("unexpectedly accepted: {value}")),
        Err(error) if error.is_remote() => (true, error.to_string()),
        Err(error) => (
            false,
            format!("transport/protocol failure is not a valid rejection: {error}"),
        ),
    };
    record_scenario(scenarios, name, passed, details)
}
