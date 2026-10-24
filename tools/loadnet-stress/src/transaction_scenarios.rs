//! Live load-network scenarios grouped by subsystem.

use super::*;

pub(super) fn run_negative_and_boundary_scenarios(
    cluster: &Cluster,
    accounts: &[Account],
    args: &Args,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<Vec<SignedTx>> {
    let nodes = cluster.rpc_nodes();
    let primary = &cluster.nodes[0].rpc;
    for (name, raw) in [
        ("raw_tx_empty_hex_rejected", ""),
        ("raw_tx_odd_hex_rejected", "0"),
        ("raw_tx_non_hex_rejected", "zz"),
        ("raw_tx_truncated_header_rejected", "00"),
    ] {
        expected_rejection(
            scenarios,
            name,
            primary.admin.call("sendrawtransaction", json!([raw])),
        )?;
        stats.rejected_expected += 1;
    }

    expected_rejection(
        scenarios,
        "insufficient_wallet_funds_rejected",
        primary.admin.call(
            "sendtoaddress",
            json!([accounts[0].address, 9_000_000_000.0]),
        ),
    )?;
    stats.rejected_expected += 1;

    let invalid_address = "GEO-not-a-canonical-address";
    let address_check = primary
        .readonly
        .call("validateaddress", json!([invalid_address]))?;
    record_scenario(
        scenarios,
        "noncanonical_address_detected",
        address_check.get("isvalid").and_then(Value::as_bool) == Some(false),
        address_check.to_string(),
    )?;
    expected_rejection(
        scenarios,
        "wallet_invalid_destination_rejected",
        primary
            .admin
            .call("sendtoaddress", json!([invalid_address, 0.001])),
    )?;
    stats.rejected_expected += 1;

    let dummy_input = json!([{
        "txid": "00".repeat(32),
        "vout": 0,
    }]);
    expected_rejection(
        scenarios,
        "raw_builder_invalid_destination_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([dummy_input.clone(), [{"address": invalid_address, "value_angles": DUST_ANGLES}]]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "raw_builder_empty_inputs_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([[], [{"address": accounts[1].address, "value_angles": DUST_ANGLES}]]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "raw_builder_empty_outputs_rejected",
        primary
            .admin
            .call("createrawtransaction", json!([dummy_input.clone(), []])),
    )?;
    expected_rejection(
        scenarios,
        "raw_builder_zero_output_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([dummy_input.clone(), [{"address": accounts[1].address, "value_angles": 0}]]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "raw_builder_vout_overflow_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([[{"txid": "00".repeat(32), "vout": u64::from(u32::MAX) + 1}], [{"address": accounts[1].address, "value_angles": DUST_ANGLES}]]),
        ),
    )?;
    expected_rejection(
        scenarios,
        "raw_builder_locktime_overflow_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([dummy_input.clone(), [{"address": accounts[1].address, "value_angles": DUST_ANGLES}], u64::from(u32::MAX) + 1]),
        ),
    )?;
    let excessive_outputs = (0..=MAX_TX_OUTPUTS)
        .map(|_| json!({"address": accounts[1].address, "value_angles": DUST_ANGLES}))
        .collect::<Vec<_>>();
    expected_rejection(
        scenarios,
        "raw_builder_output_count_over_consensus_limit_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([dummy_input.clone(), excessive_outputs]),
        ),
    )?;
    let excessive_inputs = (0..=MAX_TX_INPUTS)
        .map(|vout| json!({"txid": "00".repeat(32), "vout": vout}))
        .collect::<Vec<_>>();
    expected_rejection(
        scenarios,
        "raw_builder_input_count_over_consensus_limit_rejected",
        primary.admin.call(
            "createrawtransaction",
            json!([excessive_inputs, [{"address": accounts[1].address, "value_angles": DUST_ANGLES}]]),
        ),
    )?;
    stats.rejected_expected += 8;

    let conflict_account = &accounts[0];
    let conflict_node = &nodes[conflict_account.owner_node];
    let conflict_utxo = select_utxo(
        &conflict_node.admin,
        &conflict_account.address,
        args.fee_angles + 50_000,
    )?;
    let first = build_signed_transfer(
        conflict_node,
        &conflict_account.address,
        &accounts[1].address,
        20_000,
        args.fee_angles,
        Some(conflict_utxo.clone()),
    )?;
    let second = build_signed_transfer(
        conflict_node,
        &conflict_account.address,
        &accounts[2].address,
        30_000,
        args.fee_angles,
        Some(conflict_utxo),
    )?;
    broadcast_signed(&nodes, &first)?;
    stats.attempted += 1;
    stats.accepted += 1;
    let conflict_check = primary
        .admin
        .call("testmempoolaccept", json!([[second.hex]]))?;
    let conflict_rejected = conflict_check
        .as_array()
        .and_then(|items| items.first())
        .and_then(|item| item.get("allowed"))
        .and_then(Value::as_bool)
        == Some(false);
    record_scenario(
        scenarios,
        "mempool_double_spend_rejected",
        conflict_rejected,
        conflict_check.to_string(),
    )?;
    stats.rejected_expected += 1;

    let before_duplicate = mempool_invariant(&primary.miner)?;
    let duplicate_results = Mutex::new(Vec::new());
    thread::scope(|scope| {
        for _ in 0..32 {
            let client = primary.admin.clone();
            let raw = first.hex.clone();
            let results = &duplicate_results;
            scope.spawn(move || {
                let result = client.call("sendrawtransaction", json!([raw]));
                results
                    .lock()
                    .expect("duplicate result mutex poisoned")
                    .push(result);
            });
        }
    });
    let duplicate_results = duplicate_results
        .into_inner()
        .map_err(|_| anyhow!("duplicate result mutex poisoned"))?;
    let duplicate_responses_safe = duplicate_results.iter().all(|result| match result {
        Ok(value) => value.as_str() == Some(first.txid.as_str()),
        Err(error) => error.is_remote(),
    });
    let after_duplicate = mempool_invariant(&primary.miner)?;
    record_scenario(
        scenarios,
        "concurrent_duplicate_storm_is_idempotent",
        duplicate_responses_safe
            && transaction_known(&primary.admin, &first.txid)
            && before_duplicate == after_duplicate,
        format!(
            "requests={} before={before_duplicate:?} after={after_duplicate:?}",
            duplicate_results.len()
        ),
    )?;
    stats.rejected_expected += duplicate_results.len() as u64;

    let underfee_account = &accounts[3];
    let underfee = build_signed_transfer(
        &nodes[underfee_account.owner_node],
        &underfee_account.address,
        &accounts[4 % accounts.len()].address,
        20_000,
        1,
        None,
    )?;
    let before_underfee = mempool_invariant(&primary.miner)?;
    let underfee_check = preflight_item(&primary.admin, &underfee.hex)?;
    let underfee_rejected = underfee_check.get("allowed").and_then(Value::as_bool) == Some(false);
    let after_underfee = mempool_invariant(&primary.miner)?;
    record_scenario(
        scenarios,
        "underfee_transaction_rejected",
        underfee_rejected,
        underfee_check.to_string(),
    )?;
    record_scenario(
        scenarios,
        "rejected_preflight_has_zero_mempool_side_effects",
        before_underfee == after_underfee,
        format!("before={before_underfee:?} after={after_underfee:?}"),
    )?;
    stats.rejected_expected += 1;

    let relay_account = &accounts[9];
    let relay_tx = build_signed_transfer(
        &nodes[relay_account.owner_node],
        &relay_account.address,
        &accounts[10].address,
        20_000,
        args.fee_angles,
        None,
    )?;
    let before_dry_run = mempool_invariant(&primary.miner)?;
    let dry_run_batch = vec![relay_tx.hex.clone(); MAX_TESTMEMPOOLACCEPT_BATCH];
    let dry_run_result = primary
        .admin
        .call("testmempoolaccept", json!([dry_run_batch]))?;
    let every_allowed = dry_run_result.as_array().is_some_and(|items| {
        items.len() == MAX_TESTMEMPOOLACCEPT_BATCH
            && items
                .iter()
                .all(|item| item.get("allowed").and_then(Value::as_bool) == Some(true))
    });
    let after_dry_run = mempool_invariant(&primary.miner)?;
    record_scenario(
        scenarios,
        "testmempoolaccept_exact_batch_is_non_mutating",
        every_allowed && before_dry_run == after_dry_run,
        format!("before={before_dry_run:?} after={after_dry_run:?}"),
    )?;
    let oversized_dry_run = vec![relay_tx.hex.clone(); MAX_TESTMEMPOOLACCEPT_BATCH + 1];
    expected_rejection(
        scenarios,
        "testmempoolaccept_batch_over_limit_rejected",
        primary
            .admin
            .call("testmempoolaccept", json!([oversized_dry_run])),
    )?;
    stats.rejected_expected += 1;
    submit_single_origin_and_wait(&nodes, relay_account.owner_node, &relay_tx, cluster.timeout)?;
    record_scenario(
        scenarios,
        "single_origin_transaction_relay_converges",
        true,
        relay_tx.txid.clone(),
    )?;
    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += 1;

    let signature_account = &accounts[5];
    let signed = build_signed_transfer(
        &nodes[signature_account.owner_node],
        &signature_account.address,
        &accounts[6 % accounts.len()].address,
        20_000,
        args.fee_angles,
        None,
    )?;
    let mut corrupt = signed.hex.clone().into_bytes();
    if let Some(last) = corrupt.last_mut() {
        *last = if *last == b'0' { b'1' } else { b'0' };
    }
    let corrupt = String::from_utf8(corrupt)?;
    let bad_signature_result = primary.admin.call("testmempoolaccept", json!([[corrupt]]));
    let bad_signature_rejected = match &bad_signature_result {
        Ok(value) => {
            value
                .as_array()
                .and_then(|items| items.first())
                .and_then(|item| item.get("allowed"))
                .and_then(Value::as_bool)
                == Some(false)
        }
        Err(error) => error.is_remote(),
    };
    record_scenario(
        scenarios,
        "corrupt_signature_rejected",
        bad_signature_rejected,
        format!("{bad_signature_result:?}"),
    )?;
    stats.rejected_expected += 1;

    expected_rejection(
        scenarios,
        "raw_tx_trailing_bytes_rejected",
        primary
            .admin
            .call("sendrawtransaction", json!([format!("{}00", signed.hex)])),
    )?;
    let truncated_signed = &signed.hex[..signed.hex.len().saturating_sub(2)];
    expected_rejection(
        scenarios,
        "raw_tx_truncated_witness_rejected",
        primary
            .admin
            .call("sendrawtransaction", json!([truncated_signed])),
    )?;
    stats.rejected_expected += 2;

    let duplicate_input_account = &accounts[11];
    let duplicate_input_node = &nodes[duplicate_input_account.owner_node];
    let duplicate_utxo = select_utxo(
        &duplicate_input_node.admin,
        &duplicate_input_account.address,
        args.fee_angles + DUST_ANGLES,
    )?;
    let duplicate_value = duplicate_utxo
        .get("value")
        .and_then(Value::as_u64)
        .ok_or_else(|| anyhow!("duplicate-input UTXO has no value"))?;
    let duplicate_ref = json!({
        "txid": duplicate_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
        "vout": duplicate_utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
    });
    let duplicate_raw: String = duplicate_input_node.admin.call_as(
        "createrawtransaction",
        json!([[duplicate_ref.clone(), duplicate_ref], [{
            "address": accounts[12].address,
            "value_angles": duplicate_value - args.fee_angles,
        }]]),
    )?;
    let duplicate_signed = sign_raw(&duplicate_input_node.admin, &duplicate_raw)?;
    let (duplicate_inputs_rejected, duplicate_inputs_details) =
        preflight_rejected(&primary.admin, &duplicate_signed.hex)?;
    record_scenario(
        scenarios,
        "duplicate_inputs_inside_transaction_rejected",
        duplicate_inputs_rejected,
        duplicate_inputs_details,
    )?;
    stats.rejected_expected += 1;

    let dust_account = &accounts[12];
    let dust_node = &nodes[dust_account.owner_node];
    let dust_utxo = select_utxo(
        &dust_node.admin,
        &dust_account.address,
        args.fee_angles + DUST_ANGLES,
    )?;
    let dust_value = dust_utxo.get("value").and_then(Value::as_u64).unwrap_or(0);
    let dust_raw: String = dust_node.admin.call_as(
        "createrawtransaction",
        json!([[{
            "txid": dust_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
            "vout": dust_utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
        }], [
            {"address": accounts[13].address, "value_angles": DUST_ANGLES - 1},
            {"address": dust_account.address, "value_angles": dust_value - (DUST_ANGLES - 1) - args.fee_angles},
        ]]),
    )?;
    let dust_signed = sign_raw(&dust_node.admin, &dust_raw)?;
    let (dust_rejected, dust_details) = preflight_rejected(&primary.admin, &dust_signed.hex)?;
    record_scenario(
        scenarios,
        "dust_output_rejected",
        dust_rejected,
        dust_details,
    )?;
    stats.rejected_expected += 1;

    let overspend_account = &accounts[13];
    let overspend_node = &nodes[overspend_account.owner_node];
    let overspend_utxo = select_utxo(
        &overspend_node.admin,
        &overspend_account.address,
        args.fee_angles,
    )?;
    let overspend_value = overspend_utxo
        .get("value")
        .and_then(Value::as_u64)
        .unwrap_or(0);
    let overspend_raw: String = overspend_node.admin.call_as(
        "createrawtransaction",
        json!([[{
            "txid": overspend_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
            "vout": overspend_utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
        }], [{"address": accounts[14].address, "value_angles": overspend_value + 1}]]),
    )?;
    let overspend_signed = sign_raw(&overspend_node.admin, &overspend_raw)?;
    let (overspend_rejected, overspend_details) =
        preflight_rejected(&primary.admin, &overspend_signed.hex)?;
    record_scenario(
        scenarios,
        "output_total_above_input_rejected",
        overspend_rejected,
        overspend_details,
    )?;
    stats.rejected_expected += 1;

    let max_money_account = &accounts[14];
    let max_money_node = &nodes[max_money_account.owner_node];
    let max_money_utxo = select_utxo(
        &max_money_node.admin,
        &max_money_account.address,
        args.fee_angles,
    )?;
    let max_money_raw: String = max_money_node.admin.call_as(
        "createrawtransaction",
        json!([[{
            "txid": max_money_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
            "vout": max_money_utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
        }], [{"address": accounts[15].address, "value_angles": 2_100_000_000_000_001_u64}]]),
    )?;
    let max_money_signed = sign_raw(&max_money_node.admin, &max_money_raw)?;
    let (max_money_rejected, max_money_details) =
        preflight_rejected(&primary.admin, &max_money_signed.hex)?;
    record_scenario(
        scenarios,
        "output_total_above_max_money_rejected",
        max_money_rejected,
        max_money_details,
    )?;
    stats.rejected_expected += 1;

    let missing_account = &accounts[15];
    let missing_node = &nodes[missing_account.owner_node];
    let missing_utxo = select_utxo(
        &missing_node.admin,
        &missing_account.address,
        args.fee_angles + DUST_ANGLES,
    )?;
    let missing_raw: String = missing_node.admin.call_as(
        "createrawtransaction",
        json!([[{
            "txid": missing_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
            "vout": u64::from(u32::MAX),
        }], [{"address": accounts[16].address, "value_angles": DUST_ANGLES}]]),
    )?;
    let missing_sign = sign_raw_result(&missing_node.admin, &missing_raw)?;
    record_scenario(
        scenarios,
        "wallet_signing_reports_missing_or_spent_input",
        missing_sign.get("complete").and_then(Value::as_bool) == Some(false)
            && missing_sign
                .get("missing_inputs")
                .and_then(Value::as_u64)
                .unwrap_or(0)
                > 0,
        missing_sign.to_string(),
    )?;

    let foreign_account = &accounts[16];
    let foreign_node = &nodes[foreign_account.owner_node];
    let foreign_utxo = select_utxo(
        &foreign_node.admin,
        &foreign_account.address,
        args.fee_angles + DUST_ANGLES,
    )?;
    let foreign_value = foreign_utxo
        .get("value")
        .and_then(Value::as_u64)
        .unwrap_or(0);
    let foreign_raw: String = foreign_node.admin.call_as(
        "createrawtransaction",
        json!([[{
            "txid": foreign_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
            "vout": foreign_utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
        }], [
            {"address": accounts[17].address, "value_angles": DUST_ANGLES},
            {"address": foreign_account.address, "value_angles": foreign_value - DUST_ANGLES - args.fee_angles},
        ]]),
    )?;
    let foreign_signer = (foreign_account.owner_node + 1) % nodes.len();
    let foreign_sign = sign_raw_result(&nodes[foreign_signer].admin, &foreign_raw)?;
    record_scenario(
        scenarios,
        "foreign_wallet_cannot_sign_spend",
        foreign_sign.get("complete").and_then(Value::as_bool) == Some(false)
            && foreign_sign
                .get("errors")
                .and_then(Value::as_array)
                .is_some_and(|items| !items.is_empty()),
        foreign_sign.to_string(),
    )?;

    let race_account = &accounts[17];
    let race_node = &nodes[race_account.owner_node];
    let race_utxo = select_utxo(
        &race_node.admin,
        &race_account.address,
        args.fee_angles + 50_000,
    )?;
    let race_candidates = Arc::new(vec![
        build_signed_transfer(
            race_node,
            &race_account.address,
            &accounts[18].address,
            20_000,
            args.fee_angles,
            Some(race_utxo.clone()),
        )?,
        build_signed_transfer(
            race_node,
            &race_account.address,
            &accounts[19].address,
            30_000,
            args.fee_angles,
            Some(race_utxo),
        )?,
    ]);
    let race_results = Mutex::new(Vec::new());
    thread::scope(|scope| {
        for candidate_index in 0..race_candidates.len() {
            let client = race_node.admin.clone();
            let candidates = Arc::clone(&race_candidates);
            let results = &race_results;
            scope.spawn(move || {
                let result = client.call(
                    "sendrawtransaction",
                    json!([candidates[candidate_index].hex]),
                );
                results
                    .lock()
                    .expect("race result mutex poisoned")
                    .push((candidate_index, result));
            });
        }
    });
    let race_results = race_results
        .into_inner()
        .map_err(|_| anyhow!("race result mutex poisoned"))?;
    let winners = race_results
        .iter()
        .filter_map(|(index, result)| match result {
            Ok(value) if value.as_str() == Some(race_candidates[*index].txid.as_str()) => {
                Some(*index)
            }
            _ => None,
        })
        .collect::<Vec<_>>();
    let losers_remote = race_results.iter().all(|(index, result)| {
        winners.contains(index) || matches!(result, Err(error) if error.is_remote())
    });
    let race_passed = winners.len() == 1 && losers_remote;
    let race_winner = winners
        .first()
        .map(|index| race_candidates[*index].clone())
        .ok_or_else(|| anyhow!("same-UTXO race produced no winner: {race_results:?}"))?;
    wait_for_mempool_relay(&nodes, &race_winner.txid, &race_winner.hex, cluster.timeout)?;
    record_scenario(
        scenarios,
        "concurrent_same_utxo_race_has_exactly_one_winner",
        race_passed,
        format!("winner={} results={race_results:?}", race_winner.txid),
    )?;
    stats.attempted += 2;
    stats.accepted += 1;
    stats.rejected_expected += 1;
    stats.broadcast_copies += 1;

    let exact_dust_account = &accounts[18];
    let exact_dust = build_signed_transfer(
        &nodes[exact_dust_account.owner_node],
        &exact_dust_account.address,
        &accounts[19].address,
        DUST_ANGLES,
        args.fee_angles,
        None,
    )?;
    preflight_allowed(&nodes[exact_dust_account.owner_node].admin, &exact_dust.hex)?;
    stats.broadcast_copies += broadcast_signed(&nodes, &exact_dust)?;
    record_scenario(
        scenarios,
        "output_exactly_at_dust_limit_accepted",
        true,
        exact_dust.txid.clone(),
    )?;
    stats.attempted += 1;
    stats.accepted += 1;

    let self_account = &accounts[19];
    let self_transfer = build_signed_transfer(
        &nodes[self_account.owner_node],
        &self_account.address,
        &self_account.address,
        20_000,
        args.fee_angles,
        None,
    )?;
    preflight_allowed(&nodes[self_account.owner_node].admin, &self_transfer.hex)?;
    stats.broadcast_copies += broadcast_signed(&nodes, &self_transfer)?;
    record_scenario(
        scenarios,
        "self_transfer_with_duplicate_destination_outputs_accepted",
        true,
        self_transfer.txid.clone(),
    )?;
    stats.attempted += 1;
    stats.accepted += 1;

    // Representative medium fan-out; the exact 1000-output boundary is tested
    // separately after this batch is confirmed.
    let fanout_account = &accounts[7];
    let fanout_node = &nodes[fanout_account.owner_node];
    let fanout_utxo = select_utxo(
        &fanout_node.admin,
        &fanout_account.address,
        args.fee_angles + 32 * DUST_ANGLES,
    )?;
    let value = fanout_utxo
        .get("value")
        .and_then(Value::as_u64)
        .unwrap_or(0);
    let per_output = DUST_ANGLES;
    let mut outputs = (0..32)
        .map(|offset| {
            json!({
                "address": accounts[(8 + offset) % accounts.len()].address,
                "value_angles": per_output,
            })
        })
        .collect::<Vec<_>>();
    outputs.push(json!({
        "address": fanout_account.address,
        "value_angles": value - 32 * per_output - args.fee_angles,
    }));
    let inputs = json!([{
        "txid": fanout_utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
        "vout": fanout_utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
    }]);
    let raw: String = fanout_node
        .admin
        .call_as("createrawtransaction", json!([inputs, outputs]))?;
    let fanout = sign_raw(&fanout_node.admin, &raw)?;
    preflight_allowed(&fanout_node.admin, &fanout.hex)?;
    broadcast_signed(&nodes, &fanout)?;
    record_scenario(
        scenarios,
        "valid_33_output_fanout",
        true,
        fanout.txid.clone(),
    )?;
    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += nodes.len() as u64;

    Ok(vec![
        first,
        relay_tx,
        race_winner,
        exact_dust,
        self_transfer,
        fanout,
    ])
}

pub(super) fn run_confirmed_replay_scenario(
    cluster: &Cluster,
    confirmed: &[SignedTx],
    scenario_name: &str,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<()> {
    ensure!(
        !confirmed.is_empty(),
        "confirmed replay scenario has no transactions"
    );
    let before = cluster.mempool_sizes()?;
    let mut responses = Vec::new();
    for node in &cluster.nodes {
        let result = node
            .rpc
            .admin
            .call("sendrawtransaction", json!([confirmed[0].hex]));
        let rejected_cleanly = matches!(&result, Err(error) if error.is_remote());
        responses.push((node.rpc.index, rejected_cleanly, format!("{result:?}")));
    }
    let after = cluster.mempool_sizes()?;
    record_scenario(
        scenarios,
        scenario_name,
        responses.iter().all(|(_, passed, _)| *passed) && before == after,
        format!("before={before:?} after={after:?} responses={responses:?}"),
    )?;
    stats.rejected_expected += cluster.nodes.len() as u64;
    Ok(())
}

pub(super) fn run_transaction_limit_scenarios(
    cluster: &mut Cluster,
    accounts: &[Account],
    args: &Args,
    scenarios: &mut Vec<ScenarioResult>,
    stats: &mut TxStats,
) -> Result<()> {
    cluster.assert_all_mempools_empty()?;
    let nodes = cluster.rpc_nodes();
    let account = &accounts[20];
    let node = &nodes[account.owner_node];
    let boundary_fee = args.fanout_fee_angles.max(250_000);
    let utxo = select_utxo(
        &node.admin,
        &account.address,
        boundary_fee + (MAX_TX_OUTPUTS as u64 - 1) * DUST_ANGLES,
    )?;
    let value = utxo
        .get("value")
        .and_then(Value::as_u64)
        .ok_or_else(|| anyhow!("output-boundary UTXO has no value"))?;
    let dust_total = (MAX_TX_OUTPUTS as u64 - 1) * DUST_ANGLES;
    ensure!(
        value > dust_total + boundary_fee + DUST_ANGLES,
        "output-boundary UTXO is too small"
    );
    let mut outputs = (0..MAX_TX_OUTPUTS - 1)
        .map(|_| json!({"address": account.address, "value_angles": DUST_ANGLES}))
        .collect::<Vec<_>>();
    outputs.push(json!({
        "address": account.address,
        "value_angles": value - dust_total - boundary_fee,
    }));
    let inputs = json!([{
        "txid": utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
        "vout": utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
    }]);
    let raw: String = node
        .admin
        .call_as("createrawtransaction", json!([inputs, outputs]))?;
    let max_outputs_tx = sign_raw(&node.admin, &raw)?;
    let output_tx_size = max_outputs_tx.hex.len() / 2;
    ensure!(
        output_tx_size <= MAX_TX_SIZE,
        "exact output-boundary transaction is oversized: {output_tx_size}"
    );
    preflight_allowed(&node.admin, &max_outputs_tx.hex)?;
    submit_single_origin_and_wait(&nodes, account.owner_node, &max_outputs_tx, cluster.timeout)?;
    record_scenario(
        scenarios,
        "transaction_exact_1000_output_limit_accepted",
        true,
        format!("txid={} size={output_tx_size}", max_outputs_tx.txid),
    )?;
    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += 1;
    cluster.mine_until_mempools_empty(4)?;
    ensure!(
        confirm_transactions(
            &cluster.nodes[0].rpc.admin,
            std::slice::from_ref(&max_outputs_tx.txid),
        )? == 1,
        "1000-output transaction was not confirmed"
    );
    stats.confirmed += 1;

    let utxos = node
        .admin
        .call("getaddressutxos", json!([account.address, 0, 5000]))?;
    let mut created = utxos
        .as_array()
        .ok_or_else(|| anyhow!("getaddressutxos returned non-array"))?
        .iter()
        .filter(|item| {
            item.get("txid").and_then(Value::as_str) == Some(max_outputs_tx.txid.as_str())
        })
        .cloned()
        .collect::<Vec<_>>();
    ensure!(
        created.len() == MAX_TX_OUTPUTS,
        "expected {MAX_TX_OUTPUTS} spendable outputs, found {}",
        created.len()
    );
    created.sort_by_key(|item| {
        std::cmp::Reverse(item.get("value").and_then(Value::as_u64).unwrap_or(0))
    });

    let input_fee = args.fee_angles.max(250_000);
    let build_and_sign = |count: usize| -> Result<Value> {
        ensure!(
            count > 0 && count <= created.len(),
            "invalid input-boundary count {count}"
        );
        let selected = &created[..count];
        let mut total = 0_u64;
        let mut inputs = Vec::with_capacity(count);
        for item in selected {
            total = total
                .checked_add(item.get("value").and_then(Value::as_u64).unwrap_or(0))
                .ok_or_else(|| anyhow!("input-boundary value overflow"))?;
            inputs.push(json!({
                "txid": item.get("txid").and_then(Value::as_str).unwrap_or_default(),
                "vout": item.get("vout").and_then(Value::as_u64).unwrap_or(0),
            }));
        }
        ensure!(
            total > input_fee + DUST_ANGLES,
            "input-boundary total is too small"
        );
        let raw: String = node.admin.call_as(
            "createrawtransaction",
            json!([inputs, [{
                "address": account.address,
                "value_angles": total - input_fee,
            }]]),
        )?;
        sign_raw_result(&node.admin, &raw)
    };

    let signed_one = build_and_sign(1)?;
    let signed_two = build_and_sign(2)?;
    let size_one = signed_one
        .get("hex")
        .and_then(Value::as_str)
        .map(|hex| hex.len() / 2)
        .unwrap_or(0);
    let size_two = signed_two
        .get("hex")
        .and_then(Value::as_str)
        .map(|hex| hex.len() / 2)
        .unwrap_or(0);
    ensure!(
        size_two > size_one,
        "signed input size did not grow: one={size_one} two={size_two}"
    );
    let per_input = size_two - size_one;
    let mut boundary_count = (1 + (MAX_TX_SIZE.saturating_sub(size_one)) / per_input)
        .clamp(1, created.len().saturating_sub(1));
    let mut boundary_result = build_and_sign(boundary_count)?;
    loop {
        let size = boundary_result
            .get("hex")
            .and_then(Value::as_str)
            .map(|hex| hex.len() / 2)
            .unwrap_or(usize::MAX);
        let complete = boundary_result.get("complete").and_then(Value::as_bool) == Some(true);
        if complete && size <= MAX_TX_SIZE {
            break;
        }
        ensure!(
            boundary_count > 1,
            "no signed transaction fits the size limit"
        );
        boundary_count -= 1;
        boundary_result = build_and_sign(boundary_count)?;
    }
    loop {
        let next = build_and_sign(boundary_count + 1)?;
        let next_size = next
            .get("hex")
            .and_then(Value::as_str)
            .map(|hex| hex.len() / 2)
            .unwrap_or(usize::MAX);
        let next_complete = next.get("complete").and_then(Value::as_bool) == Some(true);
        if next_complete && next_size <= MAX_TX_SIZE {
            boundary_count += 1;
            boundary_result = next;
            continue;
        }

        let boundary_size = boundary_result
            .get("hex")
            .and_then(Value::as_str)
            .map(|hex| hex.len() / 2)
            .unwrap_or(0);
        record_scenario(
            scenarios,
            "wallet_signer_rejects_post_witness_oversize",
            next_size > MAX_TX_SIZE
                && !next_complete
                && next
                    .get("errors")
                    .and_then(Value::as_array)
                    .is_some_and(|errors| {
                        errors.iter().any(|error| {
                            error
                                .get("error")
                                .and_then(Value::as_str)
                                .is_some_and(|message| message.contains("consensus size limit"))
                        })
                    }),
            format!(
                "max_valid_inputs={boundary_count} valid_size={boundary_size} rejected_inputs={} rejected_size={next_size}",
                boundary_count + 1
            ),
        )?;
        break;
    }

    let input_hex = boundary_result
        .get("hex")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("input-boundary signer returned no hex"))?
        .to_owned();
    let input_txid = boundary_result
        .get("txid")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("input-boundary signer returned no txid"))?
        .to_owned();
    let input_tx = SignedTx {
        txid: input_txid,
        hex: input_hex,
    };
    preflight_allowed(&node.admin, &input_tx.hex)?;
    submit_single_origin_and_wait(&nodes, account.owner_node, &input_tx, cluster.timeout)?;
    record_scenario(
        scenarios,
        "largest_effective_falcon_input_boundary_accepted",
        true,
        format!("inputs={boundary_count} size={}", input_tx.hex.len() / 2),
    )?;
    stats.attempted += 1;
    stats.accepted += 1;
    stats.broadcast_copies += 1;
    cluster.mine_until_mempools_empty(4)?;
    ensure!(
        confirm_transactions(
            &cluster.nodes[0].rpc.admin,
            std::slice::from_ref(&input_tx.txid),
        )? == 1,
        "input-boundary transaction was not confirmed"
    );
    stats.confirmed += 1;
    run_confirmed_replay_scenario(
        cluster,
        &[input_tx],
        "confirmed_input_boundary_transaction_replay_rejected_by_every_node",
        scenarios,
        stats,
    )?;
    Ok(())
}
