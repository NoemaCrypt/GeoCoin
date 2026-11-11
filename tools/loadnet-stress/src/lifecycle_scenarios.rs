//! Live load-network scenarios grouped by subsystem.

use super::*;

/// `MessageHeader::MAGIC_LOADNET` (`0xD3670003`) in little-endian wire order.
pub(super) const LOADNET_P2P_MAGIC: [u8; 4] = [0x03, 0x00, 0x67, 0xD3];
/// `MessageHeader::MAGIC_MAINNET` (`0xD3670000`) in little-endian wire order.
const MAINNET_P2P_MAGIC: [u8; 4] = [0x00, 0x00, 0x67, 0xD3];

fn p2p_header(magic: [u8; 4], message_type: u8, payload_len: u32, checksum: [u8; 4]) -> Vec<u8> {
    let mut header = Vec::with_capacity(13);
    header.extend_from_slice(&magic);
    header.push(message_type);
    header.extend_from_slice(&payload_len.to_le_bytes());
    header.extend_from_slice(&checksum);
    header
}

pub(super) fn run_crash_restart_scenario(
    cluster: &mut Cluster,
    accounts: &[Account],
    scenarios: &mut Vec<ScenarioResult>,
) -> Result<usize> {
    let index = cluster.nodes.len().saturating_sub(2).max(1);
    let before_height = cluster.height(index)?;
    let before_tip: String = cluster.nodes[index]
        .rpc
        .admin
        .call_as("getblockhash", json!([before_height]))?;
    let owned = accounts
        .iter()
        .find(|account| account.owner_node == index)
        .ok_or_else(|| anyhow!("crash-restart node owns no test account"))?;

    let recovery_started = Instant::now();
    cluster.crash_restart_node(index)?;
    let recovery_ms = recovery_started.elapsed().as_millis();
    cluster.connect_full_mesh()?;
    cluster.ensure_converged(before_height)?;
    let after_tip: String = cluster.nodes[index]
        .rpc
        .admin
        .call_as("getblockhash", json!([before_height]))?;
    let utxos = cluster.nodes[index]
        .rpc
        .admin
        .call("getaddressutxos", json!([owned.address]))?;
    let health = cluster.nodes[index]
        .rpc
        .admin
        .call("gethealth", json!([]))?;
    record_scenario(
        scenarios,
        "unclean_follower_crash_recovers_wal_chain_and_wallet",
        before_tip == after_tip
            && utxos.as_array().is_some_and(|items| !items.is_empty())
            && health.get("ok").and_then(Value::as_bool) == Some(true),
        format!(
            "node={index} height={before_height} tip={after_tip} utxos={} recovery_ms={recovery_ms}",
            utxos.as_array().map_or(0, Vec::len),
        ),
    )?;
    Ok(index)
}

pub(super) fn run_reorg_and_mempool_resurrection_scenario(
    cluster: &mut Cluster,
    accounts: &[Account],
    args: &Args,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<BTreeSet<usize>> {
    ensure!(
        cluster.nodes.len() >= 4,
        "reorg scenario requires four nodes"
    );
    cluster.assert_all_mempools_empty()?;
    cluster.connect_full_mesh()?;
    let common_height = cluster.height(0)?;
    let common_tip: String = cluster.nodes[0]
        .rpc
        .admin
        .call_as("getblockhash", json!([common_height]))?;

    // Nodes 0/1 build a short branch containing a payment while nodes 2/3
    // remain at the common ancestor.
    cluster.stop_node(2, true)?;
    cluster.stop_node(3, true)?;
    let account = accounts
        .iter()
        .find(|account| account.owner_node == 1)
        .ok_or_else(|| anyhow!("branch-A wallet account is missing"))?;
    let orphaned_tx = build_signed_transfer(
        &cluster.nodes[1].rpc,
        &account.address,
        &accounts[22].address,
        40_000,
        args.fee_angles,
        None,
    )?;
    let branch_a_nodes = Arc::new(vec![
        cluster.nodes[0].rpc.clone(),
        cluster.nodes[1].rpc.clone(),
    ]);
    submit_single_origin_and_wait(&branch_a_nodes, 1, &orphaned_tx, cluster.timeout)?;
    let (_, branch_a_height) = cluster.mine_partition_blocks(0, 2, &[0, 1])?;
    let branch_a_tip: String = cluster.nodes[0]
        .rpc
        .admin
        .call_as("getblockhash", json!([branch_a_height]))?;
    ensure!(
        transaction_known(&cluster.nodes[0].rpc.admin, &orphaned_tx.txid),
        "branch-A transaction was not indexed"
    );

    // Freeze branch A, revive the untouched pair, and make a strictly longer
    // competing branch from the same common tip.
    cluster.stop_node(0, true)?;
    cluster.stop_node(1, true)?;
    cluster.start_stopped_node(2)?;
    cluster.start_stopped_node(3)?;
    ensure!(
        cluster.height(2)? == common_height && cluster.height(3)? == common_height,
        "branch-B nodes did not restart from the common ancestor"
    );
    let (_, branch_b_height) = cluster.mine_partition_blocks(2, 3, &[2, 3])?;
    let branch_b_tip: String = cluster.nodes[2]
        .rpc
        .admin
        .call_as("getblockhash", json!([branch_b_height]))?;
    ensure!(
        branch_b_height > branch_a_height,
        "competing branch is not longer"
    );

    cluster.start_stopped_node(0)?;
    cluster.start_stopped_node(1)?;
    cluster.connect_full_mesh()?;
    cluster.ensure_converged_from(2, branch_b_height)?;
    let converged_tip: String = cluster.nodes[0]
        .rpc
        .admin
        .call_as("getblockhash", json!([branch_b_height]))?;
    ensure!(
        converged_tip == branch_b_tip,
        "cluster selected the wrong competing tip"
    );
    wait_for_mempool_relay(
        &cluster.rpc_nodes(),
        &orphaned_tx.txid,
        &orphaned_tx.hex,
        cluster.timeout,
    )?;
    let active_hash_at_old_tip: String = cluster.nodes[0]
        .rpc
        .admin
        .call_as("getblockhash", json!([branch_a_height]))?;
    record_scenario(
        scenarios,
        "longer_chain_reorg_resurrects_disconnected_transaction",
        branch_a_tip != active_hash_at_old_tip
            && common_tip
                == cluster.nodes[0]
                    .rpc
                    .admin
                    .call_as::<String>("getblockhash", json!([common_height]))?
            && converged_tip == branch_b_tip,
        format!(
            "fork_height={common_height} old_tip={branch_a_tip} new_tip={branch_b_tip} resurrected_tx={}",
            orphaned_tx.txid
        ),
    )?;

    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += 1;
    cluster.mine_until_mempools_empty(4)?;
    ensure!(
        confirm_transactions(
            &cluster.nodes[0].rpc.admin,
            std::slice::from_ref(&orphaned_tx.txid),
        )? == 1,
        "resurrected transaction was not confirmed on the winning chain"
    );
    stats.confirmed += 1;
    let mut restarted = BTreeSet::new();
    restarted.extend([0, 1, 2, 3]);
    Ok(restarted)
}

pub(super) fn run_live_p2p_fail_closed_scenario(
    cluster: &Cluster,
    scenarios: &mut Vec<ScenarioResult>,
) -> Result<()> {
    ensure!(
        cluster.nodes.len() >= 4,
        "live P2P parser corpus requires four independent victims"
    );
    let mut checksum_mismatch = p2p_header(LOADNET_P2P_MAGIC, 0x00, 1, [0, 0, 0, 0]);
    checksum_mismatch.push(0x42);
    let cases = vec![
        (
            "p2p_unknown_message_type_is_banned_before_payload_read",
            cluster.nodes.len() - 1,
            "bad-message-header",
            p2p_header(LOADNET_P2P_MAGIC, 0xFF, 0, [0, 0, 0, 0]),
        ),
        (
            "p2p_wrong_network_magic_is_banned_from_header_only",
            cluster.nodes.len() - 2,
            "bad-network-magic",
            // Mainnet magic on a loadnet listener, followed by a valid PING.
            p2p_header(MAINNET_P2P_MAGIC, 0x0C, 0, [0, 0, 0, 0]),
        ),
        (
            "p2p_invalid_fixed_challenge_length_is_banned_before_allocation",
            cluster.nodes.len() - 3,
            "message-payload-limit",
            // CHALLENGE must be exactly 92 bytes. A declared three-byte body
            // is rejected from its header without waiting for or allocating it.
            p2p_header(LOADNET_P2P_MAGIC, 0x0A, 3, [0, 0, 0, 0]),
        ),
        (
            "p2p_payload_checksum_mismatch_is_banned_fail_closed",
            cluster.nodes.len() - 4,
            "bad-message-frame",
            // VERSION permits a short payload at the framing layer, so the
            // intentionally invalid checksum is the first failing condition.
            checksum_mismatch,
        ),
    ];

    for (name, victim, reason, malformed_header) in cases {
        let before = cluster.nodes[victim]
            .rpc
            .readonly
            .call("getp2ppolicystats", json!([]))?;
        let before_count = before
            .get("reject_reasons")
            .and_then(|reasons| reasons.get(reason))
            .and_then(Value::as_u64)
            .unwrap_or(0);
        let before_bans = before
            .get("active_temp_bans")
            .and_then(Value::as_u64)
            .unwrap_or(0);

        let mut stream = TcpStream::connect(("127.0.0.1", cluster.nodes[victim].rpc.p2p_port))?;
        stream.set_write_timeout(Some(Duration::from_secs(2)))?;
        stream.write_all(&malformed_header)?;
        let _ = stream.shutdown(Shutdown::Both);

        let deadline = Instant::now() + Duration::from_secs(5);
        let mut observed = None;
        while Instant::now() < deadline {
            let current = cluster.nodes[victim]
                .rpc
                .readonly
                .call("getp2ppolicystats", json!([]))?;
            let count = current
                .get("reject_reasons")
                .and_then(|reasons| reasons.get(reason))
                .and_then(Value::as_u64)
                .unwrap_or(0);
            let active_bans = current
                .get("active_temp_bans")
                .and_then(Value::as_u64)
                .unwrap_or(0);
            if count > before_count && active_bans > before_bans {
                observed = Some(current);
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        let health = cluster.nodes[victim]
            .rpc
            .admin
            .call("gethealth", json!([]))?;
        let expected_honest_peers = cluster.nodes.len().saturating_sub(1) as u64;
        let passed = observed.is_some()
            && health.get("ok").and_then(Value::as_bool) == Some(true)
            && health
                .get("authenticated_peers")
                .and_then(Value::as_u64)
                .is_some_and(|count| count >= expected_honest_peers)
            && cluster.nodes[victim].child.as_ref().is_some();
        record_scenario(
            scenarios,
            name,
            passed,
            format!(
                "victim={victim} reason={reason} before={before} after={observed:?} health={health}"
            ),
        )?;
    }
    Ok(())
}
