//! Live NFT, sidechain, query-consistency and extra payment/chaos scenarios.

use super::*;

const LOCK_AMOUNT: u64 = 5_000_000;
const TOP_UP_AMOUNT: u64 = 200_000;
const SPEND_AMOUNT: u64 = 100_000;
const MINT_AMOUNT: u64 = 1_000_000;
const NETWORK_FEE: u64 = 100_000;
const NFT_FEE_ANGLES: u64 = 100_000;

fn tool_json(tool: &Path, args: &[&str]) -> Result<Value> {
    let output = Command::new(tool)
        .args(args)
        .output()
        .with_context(|| format!("cannot execute {}", tool.display()))?;
    ensure!(
        output.status.success(),
        "signing tool failed: {}\nstdout={}\nstderr={}",
        args.join(" "),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    serde_json::from_slice(&output.stdout)
        .with_context(|| format!("signing tool returned non-JSON for {}", args.join(" ")))
}

fn dump_private_key(client: &RpcClient, address: &str) -> Result<String> {
    let value = client.call("dumpprivkey", json!([address]))?;
    value
        .as_str()
        .map(ToOwned::to_owned)
        .or_else(|| {
            value
                .get("key")
                .and_then(Value::as_str)
                .map(ToOwned::to_owned)
        })
        .ok_or_else(|| anyhow!("dumpprivkey did not return a hex key for {address}"))
}

fn submit_hex_and_confirm(
    cluster: &mut Cluster,
    origin: usize,
    tx_hex: &str,
    label: &str,
    stats: &mut TxStats,
) -> Result<String> {
    let txid = preflight_allowed(&cluster.nodes[origin].rpc.admin, tx_hex)?;
    let signed = SignedTx {
        txid: txid.clone(),
        hex: tx_hex.to_owned(),
    };
    let since_height = cluster.height(0)?;
    submit_single_origin_and_wait(&cluster.rpc_nodes(), origin, &signed, cluster.timeout)?;
    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += 1;
    cluster.mine_until_txids_in_active_chain(std::slice::from_ref(&txid), since_height, 8)?;
    ensure!(
        confirm_transactions(&cluster.nodes[0].rpc.admin, std::slice::from_ref(&txid))? == 1,
        "{label} tx {txid} was not confirmed in the active chain"
    );
    stats.confirmed += 1;
    Ok(txid)
}

fn json_tx_type(value: &Value) -> Option<i64> {
    value
        .get("type")
        .and_then(Value::as_i64)
        .or_else(|| value.get("type").and_then(Value::as_u64).map(|value| value as i64))
}

fn node_holding_address(cluster: &Cluster, accounts: &[Account], address: &str) -> Result<usize> {
    if let Some(index) = cluster
        .primary_addresses
        .iter()
        .position(|item| item == address)
    {
        return Ok(index);
    }
    accounts
        .iter()
        .find(|account| account.address == address)
        .map(|account| account.owner_node)
        .ok_or_else(|| anyhow!("no cluster wallet holds {address}"))
}

fn nft_history_len(cluster: &Cluster, nft_id: &str) -> Result<Vec<(usize, usize)>> {
    let mut lengths = Vec::new();
    for node in &cluster.nodes {
        let history = node
            .rpc
            .readonly
            .call("getnftshistory", json!([nft_id, true]))?;
        lengths.push((
            node.rpc.index,
            history.as_array().map(Vec::len).unwrap_or(0),
        ));
    }
    Ok(lengths)
}

fn submit_wallet_nft_and_mine(
    cluster: &mut Cluster,
    origin: usize,
    submitted: &Value,
    expected_type: i64,
    label: &str,
    stats: &mut TxStats,
) -> Result<String> {
    ensure!(
        json_tx_type(submitted) == Some(expected_type),
        "{label} returned unexpected type: {submitted}"
    );
    let txid = submitted
        .get("txid")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("{label} returned no txid: {submitted}"))?
        .to_owned();
    let tx_hex = submitted
        .get("tx_hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("{label} returned no tx_hex: {submitted}"))?;
    stats.attempted += 1;
    stats.accepted += 1;
    let since_height = cluster.height(0)?;
    let signed = SignedTx {
        txid: txid.clone(),
        hex: tx_hex.to_owned(),
    };
    let copies = broadcast_signed(&cluster.rpc_nodes(), &signed)?;
    stats.broadcast_copies += copies;
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        if cluster.mempool_contains(origin, &txid)?
            || cluster
                .active_chain_txids_from(0, since_height)?
                .contains(&txid)
        {
            break;
        }
        thread::sleep(Duration::from_millis(100));
    }
    if !cluster.mempool_contains(origin, &txid)?
        && !cluster
            .active_chain_txids_from(0, since_height)?
            .contains(&txid)
    {
        bail!(
            "{label} {txid} was not in mempool or active chain after broadcast: {}",
            cluster.mempool_and_template_diagnostics(&txid)?
        );
    }
    cluster.mine_until_txids_in_active_chain(std::slice::from_ref(&txid), since_height, 8)?;
    ensure!(
        confirm_transactions(&cluster.nodes[0].rpc.admin, std::slice::from_ref(&txid))? == 1,
        "{label} {txid} was not confirmed in the active chain"
    );
    let mined = cluster.nodes[0]
        .rpc
        .admin
        .call("getrawtransaction", json!([&txid, 1]))?;
    ensure!(
        json_tx_type(&mined) == Some(expected_type),
        "confirmed {label} {txid} has unexpected type: {mined}"
    );
    stats.confirmed += 1;
    Ok(txid)
}

fn wait_nft(
    cluster: &Cluster,
    nft_id: &str,
    exists: bool,
    owner: Option<&str>,
) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(30);
    let mut last = String::new();
    while Instant::now() < deadline {
        let mut ok = true;
        last.clear();
        for node in &cluster.nodes {
            let nft = node.rpc.readonly.call("getnft", json!([nft_id]))?;
            let reported_exists = nft.get("exists").and_then(Value::as_bool) == Some(true);
            let reported_owner = nft.get("owner").and_then(Value::as_str);
            last.push_str(&format!("node{}={nft};", node.rpc.index));
            if reported_exists != exists {
                ok = false;
                continue;
            }
            if let Some(expected) = owner {
                if reported_owner != Some(expected) {
                    ok = false;
                }
            }
        }
        if ok {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(200));
    }
    bail!("NFT {nft_id} did not converge (exists={exists} owner={owner:?}): {last}")
}

fn geographic_metadata(name: &str, offset: i64) -> Value {
    json!({
        "name": name,
        "description": "loadnet live NFT",
        "image_uri": "ipfs://geocoin-loadnet-nft",
        "content_hash": "ab".repeat(32),
        "geometry": {"parts": [{"exterior": [
            {"lat_ndeg": (50_450_000_i64 + offset) * 1000, "lon_ndeg": 30_520_000_000_i64},
            {"lat_ndeg": (50_450_100_i64 + offset) * 1000, "lon_ndeg": 30_520_000_000_i64},
            {"lat_ndeg": (50_450_000_i64 + offset) * 1000, "lon_ndeg": 30_520_100_000_i64},
        ], "holes": []}]},
        "location_name": "loadnet",
        "tags": ["loadnet", "live", name],
    })
}

fn run_query_consistency_scenarios(
    cluster: &Cluster,
    accounts: &[Account],
    scenarios: &mut Vec<ScenarioResult>,
) -> Result<()> {
    let infos = cluster
        .nodes
        .iter()
        .map(|node| node.rpc.readonly.call("getblockchaininfo", json!([])))
        .collect::<Result<Vec<_>, _>>()?;
    let chains: BTreeSet<String> = infos
        .iter()
        .filter_map(|info| info.get("chain").and_then(Value::as_str).map(ToOwned::to_owned))
        .collect();
    let tips: BTreeSet<String> = infos
        .iter()
        .filter_map(|info| {
            info.get("bestblockhash")
                .and_then(Value::as_str)
                .map(ToOwned::to_owned)
        })
        .collect();
    let heights: BTreeSet<u64> = infos
        .iter()
        .filter_map(|info| info.get("blocks").and_then(Value::as_u64))
        .collect();
    record_scenario(
        scenarios,
        "all_nodes_share_loadnet_tip_and_height",
        chains.len() == 1
            && chains.contains(NETWORK)
            && tips.len() == 1
            && heights.len() == 1,
        format!("chains={chains:?} tips={tips:?} heights={heights:?}"),
    )?;

    let sample = [100usize, 250, 500, 750, 999]
        .into_iter()
        .filter(|id| *id < accounts.len())
        .map(|id| &accounts[id])
        .collect::<Vec<_>>();
    let mut mismatches = Vec::new();
    for account in sample {
        let mut balances = BTreeSet::new();
        let mut utxo_values = BTreeSet::new();
        for node in &cluster.nodes {
            let balance = node
                .rpc
                .readonly
                .call("getaddressbalance", json!([&account.address]))?;
            balances.insert(
                balance
                    .get("balance_angles")
                    .and_then(Value::as_u64)
                    .unwrap_or(u64::MAX),
            );
            let utxos = node
                .rpc
                .readonly
                .call("getaddressutxos", json!([&account.address, 0, 5000]))?;
            let sum = utxos
                .as_array()
                .map(|items| {
                    items
                        .iter()
                        .map(|item| item.get("value").and_then(Value::as_u64).unwrap_or(0))
                        .sum::<u64>()
                })
                .unwrap_or(u64::MAX);
            utxo_values.insert(sum);
        }
        if balances.len() != 1 || utxo_values.len() != 1 || balances != utxo_values {
            mismatches.push(format!(
                "{} balances={balances:?} utxos={utxo_values:?}",
                account.address
            ));
        }
    }
    record_scenario(
        scenarios,
        "cross_node_address_balance_matches_utxo_sum",
        mismatches.is_empty(),
        if mismatches.is_empty() {
            "sampled funded accounts agree on every node".to_owned()
        } else {
            mismatches.join("; ")
        },
    )?;

    let peers = cluster.nodes[0]
        .rpc
        .admin
        .call("getpeerinfo", json!([]))?;
    let encrypted = peers.as_array().is_some_and(|items| {
        items.len() == cluster.nodes.len() - 1
            && items.iter().all(|peer| {
                peer.get("transport_encrypted")
                    .and_then(Value::as_bool)
                    == Some(true)
                    || peer.get("encrypted").and_then(Value::as_bool) == Some(true)
            })
    });
    record_scenario(
        scenarios,
        "authenticated_mesh_peers_are_encrypted",
        encrypted,
        peers.to_string(),
    )?;

    let fee = cluster.nodes[0]
        .rpc
        .readonly
        .call("getfeerate", json!([]))?;
    record_scenario(
        scenarios,
        "getfeerate_reports_positive_minimum",
        fee.get("min_tx_fee")
            .and_then(Value::as_u64)
            .is_some_and(|value| value > 0),
        fee.to_string(),
    )?;

    let mut address_checks = Vec::new();
    let mut addresses_ok = true;
    for (index, address) in cluster.primary_addresses.iter().enumerate() {
        let valid = cluster.nodes[index]
            .rpc
            .readonly
            .call("validateaddress", json!([address]))?;
        let mine = cluster.nodes[index]
            .rpc
            .admin
            .call("getwalletaddressinfo", json!([address]))?;
        let node_ok = valid.get("isvalid").and_then(Value::as_bool) == Some(true)
            && mine.get("ismine").and_then(Value::as_bool) == Some(true);
        addresses_ok &= node_ok;
        address_checks.push(format!("node{index} valid={valid} mine={mine}"));
    }
    record_scenario(
        scenarios,
        "validateaddress_and_wallet_ownership_for_every_primary",
        addresses_ok,
        address_checks.join("; "),
    )?;
    let invalid_address = cluster.nodes[0]
        .rpc
        .readonly
        .call("validateaddress", json!(["not-a-geocoin-address"]))?;
    record_scenario(
        scenarios,
        "validateaddress_rejects_garbage",
        invalid_address.get("isvalid").and_then(Value::as_bool) == Some(false),
        invalid_address.to_string(),
    )?;

    let mut chain_info_ok = true;
    let mut chain_info_details = Vec::new();
    for node in &cluster.nodes {
        let info = node.rpc.readonly.call("getblockchaininfo", json!([]))?;
        let count = node
            .rpc
            .readonly
            .call_as::<u64>("getblockcount", json!([]))?;
        let info_blocks = info
            .get("blocks")
            .and_then(Value::as_u64)
            .or_else(|| info.get("height").and_then(Value::as_u64));
        let hash = node
            .rpc
            .readonly
            .call_as::<String>("getblockhash", json!([count]))?;
        let best = info
            .get("bestblockhash")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let node_ok = info_blocks == Some(count) && best == hash;
        chain_info_ok &= node_ok;
        chain_info_details.push(format!(
            "node{} count={count} info_blocks={info_blocks:?} hash={hash} best={best}",
            node.rpc.index
        ));
    }
    record_scenario(
        scenarios,
        "getblockchaininfo_matches_getblockcount_and_tip_hash",
        chain_info_ok,
        chain_info_details.join("; "),
    )?;

    expected_rejection(
        scenarios,
        "unknown_txid_rejected",
        cluster.nodes[0]
            .rpc
            .readonly
            .call("getrawtransaction", json!(["00".repeat(32), 1])),
    )?;
    expected_rejection(
        scenarios,
        "future_blockhash_rejected",
        cluster.nodes[0]
            .rpc
            .readonly
            .call("getblockhash", json!([1_000_000_u64])),
    )?;
    expected_rejection(
        scenarios,
        "readonly_cannot_create_nft",
        cluster.nodes[0].rpc.readonly.call(
            "createnft",
            json!([{
                "metadata": geographic_metadata("denied", 0),
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "miner_cannot_send_wallet_funds",
        cluster.nodes[0]
            .rpc
            .miner
            .call("sendtoaddress", json!([accounts[100].address, 0.0001])),
    )?;
    expected_rejection(
        scenarios,
        "delegation_draft_rejects_non_multisig_addresses",
        cluster.nodes[0].rpc.admin.call(
            "createdelegationproofdraft",
            json!([{
                "primary_multisig_address": accounts[100].address,
                "delegated_multisig_address": accounts[101].address,
                "primary_m": 2,
                "primary_participants": [accounts[100].address, accounts[101].address],
                "delegated_m": 2,
                "delegated_participants": [accounts[102].address, accounts[103].address],
                "max_total_delegated": 1,
                "max_per_transaction": 1,
                "max_percent_of_balance": 1,
                "nonce": 1,
            }]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "registermultisig_rejects_m_greater_than_n",
        cluster.nodes[0].rpc.admin.call(
            "registermultisig",
            json!([
                FOUNDATION_MULTISIG,
                3,
                2,
                FOUNDATION_MANAGERS[0],
                FOUNDATION_MANAGERS[1]
            ]),
        ),
    )?;

    let mut genesis_ok = true;
    let mut fund_ok = true;
    let mut fund_details = Vec::new();
    for node in &cluster.nodes {
        let genesis: String = node.rpc.readonly.call_as("getblockhash", json!([0]))?;
        if genesis != GENESIS_HASH {
            genesis_ok = false;
        }
        let fund = node.rpc.readonly.call("getfundbalance", json!([]))?;
        let balance = fund.get("balance_angles").and_then(Value::as_u64).unwrap_or(0);
        let address = fund.get("address").and_then(Value::as_str).unwrap_or_default();
        fund_details.push(format!("node{} addr={address} balance={balance}", node.rpc.index));
        if address != FOUNDATION_MULTISIG || balance == 0 {
            fund_ok = false;
        }
    }
    record_scenario(
        scenarios,
        "every_node_reports_isolated_loadnet_genesis",
        genesis_ok,
        GENESIS_HASH,
    )?;
    record_scenario(
        scenarios,
        "foundation_dividend_fund_replicated_and_funded",
        fund_ok,
        fund_details.join("; "),
    )?;

    let storm = Mutex::new(Vec::new());
    thread::scope(|scope| {
        for node in &cluster.nodes {
            let results = &storm;
            scope.spawn(move || {
                let info = node.rpc.readonly.call("getblockchaininfo", json!([]));
                let mempool = node.rpc.miner.call("getmempoolinfo", json!([]));
                let peers = node.rpc.admin.call("getpeerinfo", json!([]));
                results.lock().expect("query storm mutex").push((
                    node.rpc.index,
                    info.is_ok() && mempool.is_ok() && peers.is_ok(),
                    format!("info={info:?} mempool={mempool:?} peers={peers:?}"),
                ));
            });
        }
    });
    let storm = storm.into_inner().map_err(|_| anyhow!("query storm mutex poisoned"))?;
    record_scenario(
        scenarios,
        "concurrent_readonly_queries_on_every_node",
        storm.len() == cluster.nodes.len() && storm.iter().all(|item| item.1),
        storm
            .iter()
            .map(|(index, ok, details)| format!("node{index} ok={ok} {details}"))
            .collect::<Vec<_>>()
            .join(" | "),
    )?;
    Ok(())
}

fn run_nft_live_scenarios(
    cluster: &mut Cluster,
    accounts: &[Account],
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<()> {
    let recipient = &accounts[53];
    expected_rejection(
        scenarios,
        "nft_self_intersecting_polygon_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": {
                    "name": "bowtie",
                    "content_hash": "cd".repeat(32),
                    "geometry": {"parts": [{"exterior": [
                        {"lat_ndeg": 10_000_000_000_i64, "lon_ndeg": 10_000_000_000_i64},
                        {"lat_ndeg": 10_100_000_000_i64, "lon_ndeg": 10_100_000_000_i64},
                        {"lat_ndeg": 10_000_000_000_i64, "lon_ndeg": 10_100_000_000_i64},
                        {"lat_ndeg": 10_100_000_000_i64, "lon_ndeg": 10_000_000_000_i64},
                    ], "holes": []}]},
                    "tags": ["loadnet"],
                },
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "nft_duplicate_tags_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": {
                    "name": "dup-tags",
                    "content_hash": "ef".repeat(32),
                    "geometry": {"parts": [{"exterior": [
                        {"lat_ndeg": 11_000_000_000_i64, "lon_ndeg": 11_000_000_000_i64},
                        {"lat_ndeg": 11_000_100_000_i64, "lon_ndeg": 11_000_000_000_i64},
                        {"lat_ndeg": 11_000_000_000_i64, "lon_ndeg": 11_000_100_000_i64},
                    ], "holes": []}]},
                    "tags": ["dup", "dup"],
                },
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "nft_invalid_content_hash_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": {
                    "name": "bad-hash",
                    "content_hash": "zz",
                    "geometry": {"parts": [{"exterior": [
                        {"lat_ndeg": 12_000_000_000_i64, "lon_ndeg": 12_000_000_000_i64},
                        {"lat_ndeg": 12_000_100_000_i64, "lon_ndeg": 12_000_000_000_i64},
                        {"lat_ndeg": 12_000_000_000_i64, "lon_ndeg": 12_000_100_000_i64},
                    ], "holes": []}]},
                },
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "nft_two_point_polygon_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": {
                    "name": "line",
                    "content_hash": "11".repeat(32),
                    "geometry": {"parts": [{"exterior": [
                        {"lat_ndeg": 13_000_000_000_i64, "lon_ndeg": 13_000_000_000_i64},
                        {"lat_ndeg": 13_000_100_000_i64, "lon_ndeg": 13_000_100_000_i64},
                    ], "holes": []}]},
                    "tags": ["loadnet"],
                },
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "nft_cadastral_official_area_mismatch_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": {
                    "name": "bad-cadastral",
                    "content_hash": "22".repeat(32),
                    "geometry": {"parts": [{"exterior": [
                        {"lat_ndeg": 14_000_000_000_i64, "lon_ndeg": 14_000_000_000_i64},
                        {"lat_ndeg": 14_000_100_000_i64, "lon_ndeg": 14_000_000_000_i64},
                        {"lat_ndeg": 14_000_000_000_i64, "lon_ndeg": 14_000_100_000_i64},
                    ], "holes": []}]},
                    "tags": ["cadastral", "mismatch"],
                    "cadastral": {
                        "cadastral_number": "77:01:0001001:loadnet",
                        "area_sqcm_official": 10000,
                        "has_buildings": false,
                        "building_count": 0,
                    },
                },
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "miner_cannot_create_nft",
        cluster.nodes[0].rpc.miner.call(
            "createnft",
            json!([{
                "metadata": geographic_metadata("miner-denied", 50_000),
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "nft_royalty_over_100_percent_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": geographic_metadata("royalty", 200_000),
                "royalty_percent": 10001,
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let missing_nft = cluster.nodes[0].rpc.readonly.call(
        "getnft",
        json!(["ab".repeat(32)]),
    )?;
    record_scenario(
        scenarios,
        "getnft_unknown_id_reports_missing",
        missing_nft.get("exists").and_then(Value::as_bool) == Some(false),
        missing_nft.to_string(),
    )?;
    expected_rejection(
        scenarios,
        "nft_transfer_unknown_id_rejected",
        cluster.nodes[0].rpc.admin.call(
            "transfernft",
            json!([{
                "nft_id": "ab".repeat(32),
                "to": recipient.address,
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "nft_transfer_invalid_to_rejected",
        cluster.nodes[0].rpc.admin.call(
            "transfernft",
            json!([{
                "nft_id": "ab".repeat(32),
                "to": "not-a-geocoin-address",
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "readonly_cannot_transfer_nft",
        cluster.nodes[0].rpc.readonly.call(
            "transfernft",
            json!([{
                "nft_id": "ab".repeat(32),
                "to": recipient.address,
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "miner_cannot_transfer_nft",
        cluster.nodes[0].rpc.miner.call(
            "transfernft",
            json!([{
                "nft_id": "ab".repeat(32),
                "to": recipient.address,
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "nft_cadastral_missing_number_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createnft",
            json!([{
                "metadata": {
                    "name": "no-cadastral-number",
                    "content_hash": "33".repeat(32),
                    "geometry": {"parts": [{"exterior": [
                        {"lat_ndeg": 15_000_000_000_i64, "lon_ndeg": 15_000_000_000_i64},
                        {"lat_ndeg": 15_000_100_000_i64, "lon_ndeg": 15_000_000_000_i64},
                        {"lat_ndeg": 15_000_000_000_i64, "lon_ndeg": 15_000_100_000_i64},
                    ], "holes": []}]},
                    "cadastral": {
                        "owner_name": "Loadnet Owner",
                        "area_sqcm_official": 640000,
                        "has_buildings": false,
                        "building_count": 0,
                    },
                },
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let created = cluster.nodes[0].rpc.admin.call(
        "createnft",
        json!([{
            "metadata": geographic_metadata("transferable", 0),
            "royalty_percent": 250,
            "fee_angles": NFT_FEE_ANGLES,
        }]),
    )?;
    let nft_id = created
        .get("nft_id")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("createnft returned no nft_id: {created}"))?
        .to_owned();
    let create_txid = submit_wallet_nft_and_mine(cluster, 0, &created, 10, "createnft", stats)?;
    let created_state = cluster.nodes[0]
        .rpc
        .readonly
        .call("getnft", json!([&nft_id]))?;
    let creator_owner = created_state
        .get("owner")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("created NFT has no owner: {created_state}"))?
        .to_owned();
    record_scenario(
        scenarios,
        "nft_create_spends_primary_wallet_not_imported_accounts",
        Some(creator_owner.as_str()) == cluster.primary_addresses.first().map(String::as_str),
        format!(
            "owner={creator_owner} primary={}",
            cluster.primary_addresses.first().cloned().unwrap_or_default()
        ),
    )?;
    wait_nft(cluster, &nft_id, true, Some(&creator_owner))?;
    let create_history = nft_history_len(cluster, &nft_id)?;
    ensure!(
        create_history.iter().all(|(_, len)| *len >= 1),
        "NFT create history missing after mine: {create_history:?}"
    );
    expected_rejection(
        scenarios,
        "getmempoolentry_of_confirmed_nft_create_rejected",
        cluster.nodes[0]
            .rpc
            .readonly
            .call("getmempoolentry", json!([&create_txid])),
    )?;

    let overlapping = cluster.nodes[0].rpc.admin.call(
        "createnft",
        json!([{
            "metadata": {
                "name": "overlapping-non-cadastral",
                "description": "loadnet live NFT",
                "image_uri": "ipfs://geocoin-loadnet-nft-overlap",
                "content_hash": "44".repeat(32),
                "geometry": {"parts": [{"exterior": [
                    {"lat_ndeg": 50_450_000_000_i64, "lon_ndeg": 30_520_000_000_i64},
                    {"lat_ndeg": 50_450_100_000_i64, "lon_ndeg": 30_520_000_000_i64},
                    {"lat_ndeg": 50_450_000_000_i64, "lon_ndeg": 30_520_100_000_i64},
                ], "holes": []}]},
                "location_name": "loadnet",
                "tags": ["loadnet", "overlap"],
            },
            "royalty_percent": 0,
            "fee_angles": NFT_FEE_ANGLES,
        }]),
    );
    match overlapping {
        Ok(second) => {
            let second_id = second
                .get("nft_id")
                .and_then(Value::as_str)
                .ok_or_else(|| anyhow!("overlapping createnft returned no nft_id: {second}"))?
                .to_owned();
            submit_wallet_nft_and_mine(cluster, 0, &second, 10, "overlapping createnft", stats)?;
            wait_nft(cluster, &second_id, true, None)?;
            record_scenario(
                scenarios,
                "non_cadastral_overlapping_polygon_is_allowed",
                true,
                format!("first={nft_id} second={second_id}"),
            )?;
        }
        Err(error) if error.is_remote() => {
            record_scenario(
                scenarios,
                "non_cadastral_overlapping_polygon_is_allowed",
                false,
                format!("unexpectedly rejected: {error}"),
            )?;
        }
        Err(error) => return Err(error.into()),
    }

    let owner_node = node_holding_address(cluster, accounts, &creator_owner)?;
    let transferred = cluster.nodes[owner_node].rpc.admin.call(
        "transfernft",
        json!([{
            "nft_id": nft_id,
            "to": recipient.address,
            "fee_angles": NFT_FEE_ANGLES,
        }]),
    )?;
    ensure!(
        transferred.get("to").and_then(Value::as_str) == Some(recipient.address.as_str()),
        "transfernft destination mismatch: {transferred}"
    );
    let decoded = cluster.nodes[owner_node].rpc.admin.call(
        "decoderawtransaction",
        json!([transferred
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("transfernft returned no tx_hex: {transferred}"))?]),
    )?;
    record_scenario(
        scenarios,
        "decoderawtransaction_reports_nft_transfer_payload",
        json_tx_type(&decoded) == Some(11)
            && decoded
                .pointer("/nft_transfer/to")
                .and_then(Value::as_str)
                == Some(recipient.address.as_str()),
        decoded.to_string(),
    )?;
    let transfer_txid =
        submit_wallet_nft_and_mine(cluster, owner_node, &transferred, 11, "transfernft", stats)?;
    let mined_transfer = cluster.nodes[0].rpc.admin.call(
        "getrawtransaction",
        json!([&transfer_txid, 1]),
    )?;
    ensure!(
        mined_transfer
            .pointer("/nft_transfer/to")
            .and_then(Value::as_str)
            == Some(recipient.address.as_str()),
        "confirmed NFT transfer destination mismatch: {mined_transfer}"
    );
    let transfer_history = nft_history_len(cluster, &nft_id)?;
    ensure!(
        transfer_history.iter().all(|(_, len)| *len >= 2),
        "NFT transfer did not append history on every node: {transfer_history:?}"
    );
    wait_nft(cluster, &nft_id, true, Some(&recipient.address))?;

    let mut listed_ok = true;
    let mut listed_details = Vec::new();
    let mut creator_empty = true;
    for node in &cluster.nodes {
        let owned = node
            .rpc
            .readonly
            .call("listnftsbyowner", json!([&recipient.address, 0, 50]))?;
        let count = owned.as_array().map(Vec::len).unwrap_or(0);
        listed_details.push(format!("node{}={count}", node.rpc.index));
        if count < 1 {
            listed_ok = false;
        }
        let creator_owned = node
            .rpc
            .readonly
            .call("listnftsbyowner", json!([&creator_owner, 0, 50]))?;
        let creator_has_original = creator_owned.as_array().is_some_and(|items| {
            items.iter().any(|item| {
                item.as_str() == Some(nft_id.as_str())
                    || item.get("nft_id").and_then(Value::as_str) == Some(nft_id.as_str())
            })
        });
        if creator_has_original {
            creator_empty = false;
        }
    }
    record_scenario(
        scenarios,
        "listnftsbyowner_replicates_transferred_nft",
        listed_ok,
        listed_details.join(","),
    )?;
    record_scenario(
        scenarios,
        "previous_owner_no_longer_lists_transferred_nft",
        creator_empty,
        format!("creator={creator_owner} nft_id={nft_id}"),
    )?;
    expected_rejection(
        scenarios,
        "nft_transfer_from_non_owner_wallet_rejected",
        cluster.nodes[2].rpc.admin.call(
            "transfernft",
            json!([{
                "nft_id": nft_id,
                "to": cluster.primary_addresses[2],
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let burn_node = node_holding_address(cluster, accounts, &recipient.address)?;
    let burned = cluster.nodes[burn_node].rpc.admin.call(
        "burnnft",
        json!([{
            "nft_id": nft_id,
            "fee_angles": NFT_FEE_ANGLES,
        }]),
    )?;
    let burn_txid = submit_wallet_nft_and_mine(cluster, burn_node, &burned, 12, "burnnft", stats)?;
    wait_nft(cluster, &nft_id, false, None)?;

    expected_rejection(
        scenarios,
        "burned_nft_cannot_be_transferred",
        cluster.nodes[0].rpc.admin.call(
            "transfernft",
            json!([{
                "nft_id": nft_id,
                "to": cluster.primary_addresses[2],
                "fee_angles": NFT_FEE_ANGLES,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let mut history_ok = true;
    let mut history_details = Vec::new();
    for node in &cluster.nodes {
        let history = node
            .rpc
            .readonly
            .call("getnftshistory", json!([&nft_id, true]))?;
        let len = history.as_array().map(Vec::len).unwrap_or(0);
        history_details.push(format!("node{}={len}", node.rpc.index));
        if len < 3 {
            history_ok = false;
        }
    }
    record_scenario(
        scenarios,
        "nft_create_transfer_burn_history_replicated",
        history_ok,
        history_details.join(","),
    )?;
    record_scenario(
        scenarios,
        "cross_node_nft_lifecycle_confirmed",
        true,
        format!("nft_id={nft_id} create={create_txid} transfer={transfer_txid} burn={burn_txid} owner_node={owner_node} burn_node={burn_node}"),
    )?;
    Ok(())
}

fn owner_finalize(
    cluster: &Cluster,
    tool: &Path,
    origin: usize,
    create_method: &str,
    request: Value,
    finalize_method: &str,
    private_key_hex: &str,
    with_inputs: bool,
) -> Result<Value> {
    let created = cluster.nodes[origin]
        .rpc
        .admin
        .call(create_method, json!([request]))?;
    let payload = created
        .get("payload_to_sign_hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("{create_method} returned no payload"))?;
    let signed = tool_json(
        tool,
        &[
            "sign-payload",
            "--private-key-hex",
            private_key_hex,
            "--payload-hex",
            payload,
        ],
    )?;
    let signature = signed
        .get("signature_hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("sign-payload returned no signature"))?;
    let mut finalized = cluster.nodes[origin].rpc.admin.call(
        finalize_method,
        json!([created.get("tx_hex").and_then(Value::as_str).unwrap_or(""), signature]),
    )?;
    if with_inputs {
        let tx_hex = finalized
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("{finalize_method} returned no tx_hex"))?;
        finalized = tool_json(
            tool,
            &[
                "sign-inputs",
                "--private-key-hex",
                private_key_hex,
                "--tx-hex",
                tx_hex,
            ],
        )?;
    }
    Ok(finalized)
}

fn committee_keys(tool: &Path, seeds: &[u8]) -> Result<Vec<Value>> {
    let mut keys = seeds
        .iter()
        .map(|seed| tool_json(tool, &["key-info", "--seed", &seed.to_string()]))
        .collect::<Result<Vec<_>, _>>()?;
    keys.sort_by(|left, right| {
        left.get("address")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .cmp(right.get("address").and_then(Value::as_str).unwrap_or_default())
    });
    Ok(keys)
}

fn committee_finalize(
    cluster: &Cluster,
    tool: &Path,
    origin: usize,
    create_method: &str,
    request: Value,
    finalize_method: &str,
    keys: &[Value],
) -> Result<Value> {
    let created = cluster.nodes[origin]
        .rpc
        .admin
        .call(create_method, json!([request]))?;
    let payload = created
        .get("payload_to_sign_hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("{create_method} returned no payload"))?;
    let mut signatures = Vec::new();
    for key in keys {
        let private_key = key
            .get("private_key_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("committee key missing private_key_hex"))?;
        let signed = tool_json(
            tool,
            &[
                "sign-payload",
                "--private-key-hex",
                private_key,
                "--payload-hex",
                payload,
            ],
        )?;
        signatures.push(
            signed
                .get("signature_hex")
                .and_then(Value::as_str)
                .ok_or_else(|| anyhow!("committee signature missing"))?
                .to_owned(),
        );
    }
    Ok(cluster.nodes[origin].rpc.admin.call(
        finalize_method,
        json!([
            created.get("tx_hex").and_then(Value::as_str).unwrap_or(""),
            signatures
        ]),
    )?)
}

fn funded_deposit_request(
    cluster: &Cluster,
    origin: usize,
    owner: &str,
    sidechain_id: &str,
    amount: u64,
    unlock_height: Option<u64>,
) -> Result<Value> {
    let utxo = select_utxo(
        &cluster.nodes[origin].rpc.admin,
        owner,
        amount + NETWORK_FEE,
    )?;
    let value = utxo
        .get("value")
        .and_then(Value::as_u64)
        .ok_or_else(|| anyhow!("deposit UTXO has no value"))?;
    let mut request = json!({
        "genesis_hash": sidechain_id,
        "owner_address": owner,
        "amount": amount,
        "inputs": [{
            "txid": utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
            "vout": utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
        }],
        "outputs": [{
            "address": owner,
            "value": value - amount - NETWORK_FEE,
        }],
        "validate": true,
    });
    if let Some(height) = unlock_height {
        request["unlock_height"] = json!(height);
    }
    Ok(request)
}

fn run_sidechain_live_scenarios(
    cluster: &mut Cluster,
    accounts: &[Account],
    tool: &Path,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<()> {
    expected_rejection(
        scenarios,
        "sidechain_issue_without_deposit_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createsidechainissuepayload",
            json!([{
                "genesis_hash": "11".repeat(32),
                "owner_address": accounts[50].address,
                "token_symbol": "NONE",
                "multiplier": 100,
                "committee_m": 1,
                "committee_members": [accounts[50].address],
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "sidechain_lock_without_inputs_rejected",
        cluster.nodes[0].rpc.admin.call(
            "createsidechainlockpayload",
            json!([{
                "genesis_hash": "22".repeat(32),
                "owner_address": accounts[50].address,
                "amount": 1_000,
                "unlock_height": 10,
                "inputs": [],
                "outputs": [{"address": accounts[50].address, "value": 1}],
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let unreg_owner = &accounts[50];
    let fin_owner = &accounts[51];
    let spend_recipient = &accounts[54];
    let unreg_origin = unreg_owner.owner_node;
    let fin_origin = fin_owner.owner_node;
    let unreg_key = dump_private_key(&cluster.nodes[unreg_origin].rpc.admin, &unreg_owner.address)?;
    let fin_key = dump_private_key(&cluster.nodes[fin_origin].rpc.admin, &fin_owner.address)?;

    let unreg_genesis = cluster.nodes[unreg_origin].rpc.admin.call(
        "createsidechaingenesis",
        json!([{
            "version": 1,
            "timestamp": 1_700_000_101,
            "nonce": 701,
            "chain_name": "loadnet-unregistered",
            "meta": "maximal live lock/spend/unlock",
            "sidechain_kind": "financial",
        }]),
    )?;
    let fin_genesis = cluster.nodes[fin_origin].rpc.admin.call(
        "createsidechaingenesis",
        json!([{
            "version": 1,
            "timestamp": 1_700_000_102,
            "nonce": 702,
            "chain_name": "loadnet-financial",
            "meta": "maximal live issue/mint",
            "sidechain_kind": "financial",
        }]),
    )?;
    let unreg_id = unreg_genesis
        .get("genesis_hash")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("unregistered genesis missing hash"))?
        .to_owned();
    let fin_id = fin_genesis
        .get("genesis_hash")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("financial genesis missing hash"))?
        .to_owned();
    record_scenario(
        scenarios,
        "sidechain_genesis_ids_are_unique",
        unreg_id != fin_id && unreg_id.len() == 64 && fin_id.len() == 64,
        format!("unreg={unreg_id} fin={fin_id}"),
    )?;

    let unlock_height = cluster.height(0)? + 3;
    let unreg_lock = owner_finalize(
        cluster,
        tool,
        unreg_origin,
        "createsidechainlockpayload",
        funded_deposit_request(
            cluster,
            unreg_origin,
            &unreg_owner.address,
            &unreg_id,
            LOCK_AMOUNT,
            Some(unlock_height),
        )?,
        "finalizesidechainlock",
        &unreg_key,
        true,
    )?;
    submit_hex_and_confirm(
        cluster,
        unreg_origin,
        unreg_lock
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("unregistered lock missing tx_hex"))?,
        "unregistered lock",
        stats,
    )?;

    let unreg_topup = owner_finalize(
        cluster,
        tool,
        unreg_origin,
        "createsidechaintopuppayload",
        funded_deposit_request(
            cluster,
            unreg_origin,
            &unreg_owner.address,
            &unreg_id,
            TOP_UP_AMOUNT,
            None,
        )?,
        "finalizesidechaintopup",
        &unreg_key,
        true,
    )?;
    submit_hex_and_confirm(
        cluster,
        unreg_origin,
        unreg_topup
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("unregistered top-up missing tx_hex"))?,
        "unregistered top-up",
        stats,
    )?;

    let spend = owner_finalize(
        cluster,
        tool,
        unreg_origin,
        "createsidechainspendpayload",
        json!({
            "sidechain_id": unreg_id,
            "owner_address": unreg_owner.address,
            "amount": SPEND_AMOUNT,
            "outputs": [{"address": spend_recipient.address, "value": SPEND_AMOUNT}],
            "validate": true,
        }),
        "finalizesidechainspend",
        &unreg_key,
        false,
    )?;
    submit_hex_and_confirm(
        cluster,
        unreg_origin,
        spend
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("unregistered spend missing tx_hex"))?,
        "unregistered spend",
        stats,
    )?;

    let remaining = LOCK_AMOUNT + TOP_UP_AMOUNT - SPEND_AMOUNT;
    let current = cluster.height(0)?;
    if current + 1 < unlock_height {
        cluster.mine_blocks(unlock_height.saturating_sub(current + 1))?;
    }
    let unlock = owner_finalize(
        cluster,
        tool,
        unreg_origin,
        "createsidechainunlockpayload",
        json!({
            "sidechain_id": unreg_id,
            "owner_address": unreg_owner.address,
            "outputs": [{"address": spend_recipient.address, "value": remaining}],
            "validate": true,
        }),
        "finalizesidechainunlock",
        &unreg_key,
        false,
    )?;
    submit_hex_and_confirm(
        cluster,
        unreg_origin,
        unlock
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("unregistered unlock missing tx_hex"))?,
        "unregistered unlock",
        stats,
    )?;
    expected_rejection(
        scenarios,
        "unlocked_deposit_cannot_be_spent_again",
        cluster.nodes[unreg_origin].rpc.admin.call(
            "createsidechainspendpayload",
            json!([{
                "sidechain_id": unreg_id,
                "owner_address": unreg_owner.address,
                "amount": 1_000,
                "outputs": [{"address": spend_recipient.address, "value": 1_000}],
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    record_scenario(
        scenarios,
        "unregistered_deposit_lock_topup_spend_unlock",
        true,
        format!("sidechain={unreg_id} remaining_unlocked={remaining}"),
    )?;

    let committee = committee_keys(tool, &[40, 41, 42])?;
    let members = committee
        .iter()
        .map(|key| key.get("address").and_then(Value::as_str).unwrap_or_default())
        .collect::<Vec<_>>();
    expected_rejection(
        scenarios,
        "sidechain_issue_zero_committee_threshold_rejected",
        cluster.nodes[fin_origin].rpc.admin.call(
            "createsidechainissuepayload",
            json!([{
                "genesis_hash": fin_id,
                "owner_address": fin_owner.address,
                "token_symbol": "LDNT",
                "multiplier": 100,
                "committee_m": 0,
                "committee_members": members,
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "sidechain_issue_oversized_token_symbol_rejected",
        cluster.nodes[fin_origin].rpc.admin.call(
            "createsidechainissuepayload",
            json!([{
                "genesis_hash": fin_id,
                "owner_address": fin_owner.address,
                "token_symbol": "THIS_SYMBOL_IS_TOO_LONG",
                "multiplier": 100,
                "committee_m": 2,
                "committee_members": members,
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    let fin_lock = owner_finalize(
        cluster,
        tool,
        fin_origin,
        "createsidechainlockpayload",
        funded_deposit_request(
            cluster,
            fin_origin,
            &fin_owner.address,
            &fin_id,
            LOCK_AMOUNT,
            Some(cluster.height(0)? + 10_000),
        )?,
        "finalizesidechainlock",
        &fin_key,
        true,
    )?;
    submit_hex_and_confirm(
        cluster,
        fin_origin,
        fin_lock
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("financial lock missing tx_hex"))?,
        "financial lock",
        stats,
    )?;
    let issue = owner_finalize(
        cluster,
        tool,
        fin_origin,
        "createsidechainissuepayload",
        json!({
            "genesis_hash": fin_id,
            "owner_address": fin_owner.address,
            "token_symbol": "LDNT",
            "multiplier": 100,
            "fee_bps": 100,
            "max_multiplier": 200,
            "committee_m": 2,
            "committee_members": members,
            "validate": true,
        }),
        "finalizesidechainissue",
        &fin_key,
        false,
    )?;
    submit_hex_and_confirm(
        cluster,
        fin_origin,
        issue
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("financial issue missing tx_hex"))?,
        "financial issue",
        stats,
    )?;

    let mut registry_ok = true;
    let mut registry_details = Vec::new();
    for node in &cluster.nodes {
        let registry = node
            .rpc
            .readonly
            .call("getsidechaingenesis", json!([&fin_id]))?;
        let symbol = registry.get("token_symbol").and_then(Value::as_str);
        let issued = registry.get("total_issued").and_then(Value::as_u64);
        registry_details.push(format!("node{}={registry}", node.rpc.index));
        if symbol != Some("LDNT") || issued != Some(0) {
            registry_ok = false;
        }
    }
    record_scenario(
        scenarios,
        "financial_registry_replicated_before_mint",
        registry_ok,
        registry_details.join(" | "),
    )?;

    let unsigned_mint = cluster.nodes[fin_origin].rpc.admin.call(
        "createsidechainmintpayload",
        json!([{
            "sidechain_id": fin_id,
            "owner_address": fin_owner.address,
            "amount": MINT_AMOUNT,
            "validate": true,
        }]),
    )?;
    let unsigned_hex = unsigned_mint
        .get("tx_hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("unsigned mint payload missing tx_hex"))?;
    let (unsigned_rejected, unsigned_details) =
        preflight_rejected(&cluster.nodes[0].rpc.admin, unsigned_hex)?;
    record_scenario(
        scenarios,
        "mint_without_committee_signatures_rejected_at_preflight",
        unsigned_rejected,
        unsigned_details,
    )?;
    stats.rejected_expected += 1;

    let one_sig = committee_finalize(
        cluster,
        tool,
        fin_origin,
        "createsidechainmintpayload",
        json!({
            "sidechain_id": fin_id,
            "owner_address": fin_owner.address,
            "amount": MINT_AMOUNT,
            "validate": true,
        }),
        "finalizesidechainmint",
        &committee[..1],
    )?;
    let one_sig_hex = one_sig
        .get("tx_hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("one-sig mint missing tx_hex"))?;
    let (one_sig_rejected, one_sig_details) =
        preflight_rejected(&cluster.nodes[0].rpc.admin, one_sig_hex)?;
    record_scenario(
        scenarios,
        "mint_with_insufficient_committee_quorum_rejected_at_preflight",
        one_sig_rejected,
        one_sig_details,
    )?;
    stats.rejected_expected += 1;

    let issuance_limit = cluster.nodes[0]
        .rpc
        .readonly
        .call("getsidechaingenesis", json!([&fin_id]))?
        .get("issuance_limit")
        .and_then(Value::as_u64)
        .unwrap_or(0);
    expected_rejection(
        scenarios,
        "mint_above_issuance_limit_rejected",
        cluster.nodes[fin_origin].rpc.admin.call(
            "createsidechainmintpayload",
            json!([{
                "sidechain_id": fin_id,
                "owner_address": fin_owner.address,
                "amount": issuance_limit.saturating_add(1).max(MINT_AMOUNT + 1),
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let mint = committee_finalize(
        cluster,
        tool,
        fin_origin,
        "createsidechainmintpayload",
        json!({
            "sidechain_id": fin_id,
            "owner_address": fin_owner.address,
            "amount": MINT_AMOUNT,
            "validate": true,
        }),
        "finalizesidechainmint",
        &committee[..2],
    )?;
    submit_hex_and_confirm(
        cluster,
        fin_origin,
        mint.get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("financial mint missing tx_hex"))?,
        "financial mint",
        stats,
    )?;
    let mut minted_ok = true;
    for node in &cluster.nodes {
        let registry = node
            .rpc
            .readonly
            .call("getsidechaingenesis", json!([&fin_id]))?;
        if registry.get("total_issued").and_then(Value::as_u64) != Some(MINT_AMOUNT) {
            minted_ok = false;
        }
    }
    record_scenario(
        scenarios,
        "financial_mint_replicated_on_every_node",
        minted_ok,
        format!("sidechain={fin_id} minted={MINT_AMOUNT}"),
    )?;
    expected_rejection(
        scenarios,
        "duplicate_financial_issue_rejected",
        cluster.nodes[fin_origin].rpc.admin.call(
            "createsidechainissuepayload",
            json!([{
                "genesis_hash": fin_id,
                "owner_address": fin_owner.address,
                "token_symbol": "LDNT",
                "multiplier": 100,
                "committee_m": 2,
                "committee_members": members,
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;
    expected_rejection(
        scenarios,
        "unregistered_sidechain_registry_query_rejected",
        cluster.nodes[0]
            .rpc
            .readonly
            .call("getsidechaingenesis", json!([&unreg_id])),
    )?;
    expected_rejection(
        scenarios,
        "dumpprivkey_rejected_on_non_owning_node",
        cluster.nodes[(unreg_origin + 1) % cluster.nodes.len()]
            .rpc
            .admin
            .call("dumpprivkey", json!([&unreg_owner.address])),
    )?;

    let acc_owner = &accounts[52];
    let acc_origin = acc_owner.owner_node;
    let acc_key = dump_private_key(&cluster.nodes[acc_origin].rpc.admin, &acc_owner.address)?;
    let acc_committee = committee_keys(tool, &[50, 51, 52])?;
    let acc_members = acc_committee
        .iter()
        .map(|key| key.get("address").and_then(Value::as_str).unwrap_or_default())
        .collect::<Vec<_>>();
    expected_rejection(
        scenarios,
        "accounting_genesis_requires_v2_and_schema_hash",
        cluster.nodes[acc_origin].rpc.admin.call(
            "createsidechaingenesis",
            json!([{
                "version": 1,
                "timestamp": 1_700_000_103,
                "nonce": 703,
                "chain_name": "loadnet-accounting",
                "meta": "maximal live accounting registry",
                "sidechain_kind": "accounting",
            }]),
        ),
    )?;
    let acc_genesis = cluster.nodes[acc_origin].rpc.admin.call(
        "createsidechaingenesis",
        json!([{
            "version": 2,
            "timestamp": 1_700_000_103,
            "nonce": 703,
            "chain_name": "loadnet-accounting",
            "meta": "maximal live accounting registry",
            "sidechain_kind": "accounting",
            "schema_hash": "44".repeat(32),
        }]),
    )?;
    let acc_id = acc_genesis
        .get("genesis_hash")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("accounting genesis missing hash"))?
        .to_owned();
    let acc_lock = owner_finalize(
        cluster,
        tool,
        acc_origin,
        "createsidechainlockpayload",
        funded_deposit_request(
            cluster,
            acc_origin,
            &acc_owner.address,
            &acc_id,
            LOCK_AMOUNT,
            Some(cluster.height(0)? + 10_000),
        )?,
        "finalizesidechainlock",
        &acc_key,
        true,
    )?;
    submit_hex_and_confirm(
        cluster,
        acc_origin,
        acc_lock
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("accounting lock missing tx_hex"))?,
        "accounting lock",
        stats,
    )?;
    let acc_issue = owner_finalize(
        cluster,
        tool,
        acc_origin,
        "createaccountingsidechainpayload",
        json!({
            "genesis_hash": acc_id,
            "owner_address": acc_owner.address,
            "namespace": "loadnet.land.v1",
            "schema_hash": "44".repeat(32),
            "committee_m": 2,
            "committee_members": acc_members,
            "validate": true,
        }),
        "finalizeaccountingsidechain",
        &acc_key,
        false,
    )?;
    submit_hex_and_confirm(
        cluster,
        acc_origin,
        acc_issue
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("accounting register missing tx_hex"))?,
        "accounting register",
        stats,
    )?;
    let mut accounting_ok = true;
    let mut accounting_details = Vec::new();
    for node in &cluster.nodes {
        let registry = node
            .rpc
            .readonly
            .call("getsidechaingenesis", json!([&acc_id]))?;
        let kind = registry.get("sidechain_kind").and_then(Value::as_str);
        let namespace = registry.get("namespace").and_then(Value::as_str);
        accounting_details.push(format!("node{}={registry}", node.rpc.index));
        if kind != Some("accounting") || namespace != Some("loadnet.land.v1") {
            accounting_ok = false;
        }
    }
    record_scenario(
        scenarios,
        "accounting_registry_replicated_on_every_node",
        accounting_ok,
        accounting_details.join(" | "),
    )?;
    expected_rejection(
        scenarios,
        "accounting_profile_rejects_financial_mint",
        cluster.nodes[acc_origin].rpc.admin.call(
            "createsidechainmintpayload",
            json!([{
                "sidechain_id": acc_id,
                "owner_address": acc_owner.address,
                "amount": 1,
                "validate": true,
            }]),
        ),
    )?;
    stats.rejected_expected += 1;

    let redeem = committee_finalize(
        cluster,
        tool,
        fin_origin,
        "createsidechainredeempayload",
        json!({
            "sidechain_id": fin_id,
            "owner_address": fin_owner.address,
            "amount": MINT_AMOUNT,
            "outputs": [{"address": spend_recipient.address, "value": MINT_AMOUNT}],
            "validate": true,
        }),
        "finalizesidechainredeem",
        &committee[..2],
    )?;
    submit_hex_and_confirm(
        cluster,
        fin_origin,
        redeem
            .get("tx_hex")
            .and_then(Value::as_str)
            .ok_or_else(|| anyhow!("financial redeem missing tx_hex"))?,
        "financial redeem",
        stats,
    )?;
    let mut redeemed_ok = true;
    for node in &cluster.nodes {
        let registry = node
            .rpc
            .readonly
            .call("getsidechaingenesis", json!([&fin_id]))?;
        if registry.get("total_issued").and_then(Value::as_u64) != Some(0) {
            redeemed_ok = false;
        }
    }
    record_scenario(
        scenarios,
        "financial_redeem_returns_issuance_to_zero_on_every_node",
        redeemed_ok,
        format!("sidechain={fin_id}"),
    )?;
    Ok(())
}

fn run_extra_payment_and_mining_scenarios(
    cluster: &mut Cluster,
    accounts: &[Account],
    args: &Args,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<()> {
    let ring = [60usize, 61, 62, 63];
    let mut ring_txids = Vec::new();
    let ring_since = cluster.height(0)?;
    for (index, sender_id) in ring.iter().copied().enumerate() {
        let recipient_id = ring[(index + 1) % ring.len()];
        let sender = &accounts[sender_id];
        let tx = build_signed_transfer(
            &cluster.nodes[sender.owner_node].rpc,
            &sender.address,
            &accounts[recipient_id].address,
            25_000,
            args.fee_angles,
            None,
        )?;
        submit_single_origin_and_wait(
            &cluster.rpc_nodes(),
            sender.owner_node,
            &tx,
            cluster.timeout,
        )?;
        stats.attempted += 1;
        stats.accepted += 1;
        stats.broadcast_copies += 1;
        ring_txids.push(tx.txid);
    }
    cluster.mine_blocks_from(1, 1)?;
    cluster.mine_until_txids_in_active_chain(&ring_txids, ring_since, 6)?;
    let confirmed = confirm_transactions(&cluster.nodes[0].rpc.admin, &ring_txids)?;
    record_scenario(
        scenarios,
        "four_node_circular_payments_confirmed_by_non_primary_miner",
        confirmed == ring_txids.len() as u64,
        format!("confirmed={confirmed}/{} miner=node-1", ring_txids.len()),
    )?;
    stats.confirmed += confirmed;

    let burst_since = cluster.height(0)?;
    let burst = Mutex::new(Vec::new());
    thread::scope(|scope| {
        for node_index in 0..cluster.nodes.len() {
            let sender = &accounts[70 + node_index];
            let recipient = &accounts[74 + node_index];
            let node = &cluster.nodes[sender.owner_node].rpc;
            let results = &burst;
            let fee = args.fee_angles;
            scope.spawn(move || {
                let result = build_signed_transfer(
                    node,
                    &sender.address,
                    &recipient.address,
                    15_000,
                    fee,
                    None,
                )
                .and_then(|tx| {
                    node.admin
                        .call("sendrawtransaction", json!([tx.hex]))
                        .map(|value| (tx, value))
                        .map_err(|error| anyhow!("{error}"))
                });
                results
                    .lock()
                    .expect("burst mutex poisoned")
                    .push((node_index, result));
            });
        }
    });
    let burst = burst
        .into_inner()
        .map_err(|_| anyhow!("burst mutex poisoned"))?;
    let mut burst_txids = Vec::new();
    let mut burst_ok = true;
    for (node_index, result) in &burst {
        match result {
            Ok((tx, value)) if value.as_str() == Some(tx.txid.as_str()) => {
                stats.attempted += 1;
                stats.accepted += 1;
                // A successful submission only admits the transaction on its
                // origin. Let P2P deliver the burst before fast loadnet mining
                // consumes the confirmation block budget.
                wait_for_mempool_relay(&cluster.rpc_nodes(), &tx.txid, &tx.hex, cluster.timeout)?;
                burst_txids.push(tx.txid.clone());
            }
            Ok((tx, value)) => {
                burst_ok = false;
                eprintln!(
                    "[loadnet] concurrent burst from node {node_index} returned {} vs {value}",
                    tx.txid
                );
            }
            Err(error) => {
                burst_ok = false;
                eprintln!("[loadnet] concurrent burst from node {node_index} failed: {error:#}");
            }
        }
    }
    cluster.mine_until_txids_in_active_chain(&burst_txids, burst_since, 8)?;
    let burst_confirmed = confirm_transactions(&cluster.nodes[0].rpc.admin, &burst_txids)?;
    record_scenario(
        scenarios,
        "concurrent_four_node_independent_spends",
        burst_ok && burst_confirmed == burst_txids.len() as u64 && burst_txids.len() == 4,
        format!("confirmed={burst_confirmed}/{}", burst_txids.len()),
    )?;
    stats.confirmed += burst_confirmed;

    let mesh_sender = &accounts[78];
    let mesh_tx = build_signed_transfer(
        &cluster.nodes[mesh_sender.owner_node].rpc,
        &mesh_sender.address,
        &accounts[79].address,
        12_000,
        args.fee_angles,
        None,
    )?;
    let copies = broadcast_signed(&cluster.rpc_nodes(), &mesh_tx)?;
    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += copies;
    let mesh_since = cluster.height(0)?;
    cluster.mine_blocks_from(2, 1)?;
    cluster.mine_until_txids_in_active_chain(std::slice::from_ref(&mesh_tx.txid), mesh_since, 6)?;
    let mesh_confirmed = confirm_transactions(
        &cluster.nodes[0].rpc.admin,
        std::slice::from_ref(&mesh_tx.txid),
    )?;
    record_scenario(
        scenarios,
        "idempotent_four_node_broadcast_confirmed_by_third_miner",
        copies == cluster.nodes.len() as u64 && mesh_confirmed == 1,
        format!("copies={copies} confirmed={mesh_confirmed} miner=node-2 txid={}", mesh_tx.txid),
    )?;
    stats.confirmed += mesh_confirmed;
    cluster.assert_all_mempools_empty()?;
    record_scenario(
        scenarios,
        "feature_scenarios_leave_empty_mempools",
        true,
        "all node mempools empty after NFT/sidechain/payment chaos",
    )?;
    Ok(())
}

pub(super) fn run_maximal_feature_scenarios(
    cluster: &mut Cluster,
    accounts: &[Account],
    args: &Args,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<()> {
    ensure!(
        accounts.len() > 80,
        "maximal feature scenarios need reserved accounts 50..=79"
    );
    run_query_consistency_scenarios(cluster, accounts, scenarios)?;
    run_nft_live_scenarios(cluster, accounts, scenarios, stats)?;
    run_sidechain_live_scenarios(cluster, accounts, &args.signing_tool, scenarios, stats)?;
    run_extra_payment_and_mining_scenarios(cluster, accounts, args, scenarios, stats)?;
    cluster.verify_all_accounts_funded(&accounts[80..], args.funding_angles)?;

    let miner = 0usize;
    let before_height = cluster.height(miner)?;
    let before_tip: String = cluster.nodes[miner]
        .rpc
        .admin
        .call_as("getblockhash", json!([before_height]))?;
    cluster.restart_node(miner)?;
    cluster.connect_full_mesh()?;
    cluster.ensure_converged(before_height)?;
    let after_tip: String = cluster.nodes[miner]
        .rpc
        .admin
        .call_as("getblockhash", json!([before_height]))?;
    let wallet_balance = cluster.nodes[miner]
        .rpc
        .admin
        .call("getbalance", json!([]))?;
    record_scenario(
        scenarios,
        "primary_miner_restart_keeps_tip_and_wallet",
        after_tip == before_tip
            && wallet_balance
                .as_f64()
                .or_else(|| wallet_balance.as_u64().map(|value| value as f64))
                .is_some_and(|value| value > 0.0),
        format!("height={before_height} tip={after_tip} balance={wallet_balance}"),
    )?;
    let probe = accounts
        .iter()
        .find(|account| account.owner_node == miner && account.id >= 80)
        .ok_or_else(|| anyhow!("no load account owned by restarted miner"))?;
    let miner_utxos = cluster.nodes[miner]
        .rpc
        .admin
        .call("getaddressutxos", json!([&probe.address, 0, 5000]))?;
    let follower_utxos = cluster.nodes[1]
        .rpc
        .admin
        .call("getaddressutxos", json!([&probe.address, 0, 5000]))?;
    let sum = |value: &Value| {
        value
            .as_array()
            .map(|items| {
                items
                    .iter()
                    .map(|utxo| utxo.get("value").and_then(Value::as_u64).unwrap_or(0))
                    .sum::<u64>()
            })
            .unwrap_or(0)
    };
    record_scenario(
        scenarios,
        "restarted_miner_utxo_set_matches_followers",
        sum(&miner_utxos) == sum(&follower_utxos) && sum(&follower_utxos) >= args.funding_angles,
        format!(
            "address={} miner_sum={} follower_sum={}",
            probe.address,
            sum(&miner_utxos),
            sum(&follower_utxos)
        ),
    )?;
    Ok(())
}
