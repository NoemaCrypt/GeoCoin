//! Live load-network scenarios grouped by subsystem.

use super::*;

fn tcp_connection_is_waiting(stream: &TcpStream) -> Result<bool> {
    stream.set_nonblocking(true)?;
    let mut byte = [0u8; 1];
    let result = matches!(
        stream.peek(&mut byte),
        Err(error) if error.kind() == ErrorKind::WouldBlock
    );
    stream.set_nonblocking(false)?;
    Ok(result)
}

fn tcp_connection_is_closed(stream: &TcpStream) -> Result<bool> {
    stream.set_nonblocking(true)?;
    let mut byte = [0u8; 1];
    let result = match stream.peek(&mut byte) {
        Ok(0) => true,
        Err(error)
            if matches!(
                error.kind(),
                ErrorKind::ConnectionAborted
                    | ErrorKind::ConnectionReset
                    | ErrorKind::BrokenPipe
                    | ErrorKind::NotConnected
            ) =>
        {
            true
        }
        _ => false,
    };
    stream.set_nonblocking(false)?;
    Ok(result)
}

fn open_stalled_rpc_connection(port: u16, request: &[u8]) -> Result<TcpStream> {
    let mut stream = TcpStream::connect(("127.0.0.1", port))?;
    stream.set_nodelay(true)?;
    stream.set_write_timeout(Some(Duration::from_secs(2)))?;
    stream.write_all(request)?;
    Ok(stream)
}

fn rpc_connection_cap_rejects_probe(port: u16) -> Result<bool> {
    let mut stream = TcpStream::connect(("127.0.0.1", port))?;
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    stream.set_write_timeout(Some(Duration::from_secs(2)))?;
    let request = format!("GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n");
    if stream.write_all(request.as_bytes()).is_err() {
        return Ok(true);
    }
    let _ = stream.shutdown(Shutdown::Write);
    let mut byte = [0u8; 1];
    match stream.read(&mut byte) {
        Ok(0) => Ok(true),
        Ok(_) => Ok(false),
        Err(error)
            if matches!(
                error.kind(),
                ErrorKind::ConnectionAborted
                    | ErrorKind::ConnectionReset
                    | ErrorKind::BrokenPipe
                    | ErrorKind::NotConnected
            ) =>
        {
            Ok(true)
        }
        Err(error) if matches!(error.kind(), ErrorKind::TimedOut | ErrorKind::WouldBlock) => {
            Ok(false)
        }
        Err(error) => Err(error.into()),
    }
}

fn run_live_rpc_connection_resilience_scenarios(
    cluster: &Cluster,
    scenarios: &mut Vec<ScenarioResult>,
) -> Result<()> {
    let node = &cluster.nodes[0];
    let password = fs::read_to_string(&node.readonly_password_file)?;
    let authorization = basic_auth_base64(&node.readonly_user, password.trim_end());
    let header_stall = format!(
        "POST / HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nX-Stall: header",
        node.rpc.rpc_port
    );
    let body_stall = format!(
        concat!(
            "POST / HTTP/1.1\r\n",
            "Host: 127.0.0.1:{}\r\n",
            "Authorization: Basic {}\r\n",
            "Content-Type: application/json\r\n",
            "Content-Length: 65536\r\n",
            "Connection: close\r\n\r\n{{"
        ),
        node.rpc.rpc_port, authorization
    );
    drop(authorization);
    drop(password);

    cluster.expected_monitor_outages[node.rpc.index].store(true, Ordering::Release);
    let exercise = (|| -> Result<(bool, usize, usize)> {
        let mut stalled: Vec<(bool, TcpStream)> = Vec::new();
        for _ in 0..20 {
            stalled.clear();
            for is_header in [true, true, false, false] {
                let request = if is_header {
                    header_stall.as_bytes()
                } else {
                    body_stall.as_bytes()
                };
                stalled.push((
                    is_header,
                    open_stalled_rpc_connection(node.rpc.rpc_port, request)?,
                ));
            }
            thread::sleep(Duration::from_millis(250));
            if stalled
                .iter()
                .all(|(_, stream)| tcp_connection_is_waiting(stream).unwrap_or(false))
            {
                break;
            }
            stalled.clear();
            thread::sleep(Duration::from_millis(150));
        }
        ensure!(
            stalled.len() == 4
                && stalled
                    .iter()
                    .all(|(_, stream)| tcp_connection_is_waiting(stream).unwrap_or(false)),
            "could not occupy all four per-IP RPC connection slots with stalled clients"
        );

        let cap_rejected = rpc_connection_cap_rejects_probe(node.rpc.rpc_port)?;
        thread::sleep(Duration::from_secs(17));
        let closed_headers = stalled
            .iter()
            .filter(|(is_header, stream)| {
                *is_header && tcp_connection_is_closed(stream).unwrap_or(false)
            })
            .count();
        let closed_bodies = stalled
            .iter()
            .filter(|(is_header, stream)| {
                !*is_header && tcp_connection_is_closed(stream).unwrap_or(false)
            })
            .count();
        Ok((cap_rejected, closed_headers, closed_bodies))
    })();
    cluster.expected_monitor_outages[node.rpc.index].store(false, Ordering::Release);
    let (cap_rejected, closed_headers, closed_bodies) = exercise?;

    record_scenario(
        scenarios,
        "rpc_per_ip_connection_cap_disabled_accepts_fifth_client",
        !cap_rejected,
        "default rpc-max-connections-per-ip=0 leaves extra loopback clients unconstrained",
    )?;
    record_scenario(
        scenarios,
        "rpc_partial_header_and_body_deadlines_close_stalled_clients",
        closed_headers == 2 && closed_bodies == 2,
        format!(
            "closed_partial_headers={closed_headers}/2 closed_partial_bodies={closed_bodies}/2"
        ),
    )?;

    thread::sleep(Duration::from_millis(250));
    let health = node.rpc.admin.call("gethealth", json!([]))?;
    record_scenario(
        scenarios,
        "rpc_connection_capacity_recovers_after_stall_deadlines",
        health.get("ok").and_then(Value::as_bool) == Some(true),
        health.to_string(),
    )
}

pub(super) fn run_live_rpc_security_scenarios(
    cluster: &Cluster,
    scenarios: &mut Vec<ScenarioResult>,
) -> Result<()> {
    let node = &cluster.nodes[0].rpc;
    let readonly_height = node.readonly.call("getblockcount", json!([]));
    record_scenario(
        scenarios,
        "rpc_readonly_can_read_chain",
        readonly_height.as_ref().is_ok_and(Value::is_number),
        format!("{readonly_height:?}"),
    )?;

    let readonly_stop = node.readonly.call("stop", json!([]));
    record_scenario(
        scenarios,
        "rpc_readonly_cannot_stop_node",
        matches!(&readonly_stop, Err(error) if error.remote_code() == Some(-2)),
        format!("{readonly_stop:?}"),
    )?;

    let readonly_wallet = node.readonly.call("getnewaddress", json!(["forbidden"]));
    record_scenario(
        scenarios,
        "rpc_readonly_cannot_mutate_wallet",
        matches!(&readonly_wallet, Err(error) if error.remote_code() == Some(-2)),
        format!("{readonly_wallet:?}"),
    )?;

    let miner_wallet = node.miner.call("getnewaddress", json!(["forbidden"]));
    record_scenario(
        scenarios,
        "rpc_miner_cannot_access_wallet",
        matches!(&miner_wallet, Err(error) if error.remote_code() == Some(-2)),
        format!("{miner_wallet:?}"),
    )?;

    let miner_mempool = node.miner.call("getmempoolinfo", json!([]));
    record_scenario(
        scenarios,
        "rpc_miner_can_read_mempool",
        miner_mempool
            .as_ref()
            .is_ok_and(|value| value.get("size").is_some()),
        format!("{miner_mempool:?}"),
    )?;

    let body = json!({
        "jsonrpc": "2.0",
        "id": "auth-live",
        "method": "getblockcount",
        "params": [],
    })
    .to_string();
    let no_auth = node.readonly.raw_json_post(&body, false)?;
    record_scenario(
        scenarios,
        "rpc_missing_credentials_http_401",
        no_auth.status == 401,
        format!("HTTP {} body={}", no_auth.status, no_auth.body),
    )?;

    let wrong = RpcClient::new(
        format!("http://127.0.0.1:{}/", node.rpc_port),
        "wrong-user".to_owned(),
        SECRET_LOG_CANARY.to_owned(),
        Duration::from_secs(5),
    )?;
    let wrong_auth = wrong.raw_json_post(&body, true)?;
    record_scenario(
        scenarios,
        "rpc_wrong_credentials_http_401",
        wrong_auth.status == 401 && !wrong_auth.body.contains(SECRET_LOG_CANARY),
        format!("HTTP {} body={}", wrong_auth.status, wrong_auth.body),
    )?;

    let malformed_body =
        format!("{{\"jsonrpc\":\"2.0\",\"method\":\"help\",\"params\":[\"{SECRET_LOG_CANARY}\"");
    let malformed = node.readonly.raw_json_post(&malformed_body, true)?;
    let malformed_json: Value = serde_json::from_str(&malformed.body)
        .with_context(|| format!("malformed JSON response was not JSON: {}", malformed.body))?;
    let parse_error = malformed_json
        .get("error")
        .and_then(|error| error.get("code"))
        .and_then(Value::as_i64)
        == Some(-32700);
    record_scenario(
        scenarios,
        "rpc_parse_error_is_generic_and_secret_free",
        malformed.status == 200 && parse_error && !malformed.body.contains(SECRET_LOG_CANARY),
        format!("HTTP {} body={}", malformed.status, malformed.body),
    )?;

    let valid_batch = (0..RPC_BATCH_LIMIT)
        .map(|id| {
            json!({
                "jsonrpc": "2.0",
                "id": id,
                "method": "getblockcount",
                "params": [],
            })
        })
        .collect::<Vec<_>>();
    let valid_batch_response = node
        .readonly
        .raw_json_post(&Value::Array(valid_batch).to_string(), true)?;
    let valid_batch_json: Value = serde_json::from_str(&valid_batch_response.body)?;
    record_scenario(
        scenarios,
        "rpc_batch_exact_limit_accepted",
        valid_batch_response.status == 200
            && valid_batch_json
                .as_array()
                .is_some_and(|items| items.len() == RPC_BATCH_LIMIT),
        valid_batch_response.body,
    )?;

    let oversized_batch = (0..=RPC_BATCH_LIMIT)
        .map(|id| {
            json!({
                "jsonrpc": "2.0",
                "id": id,
                "method": "getblockcount",
                "params": [],
            })
        })
        .collect::<Vec<_>>();
    let oversized_response = node
        .readonly
        .raw_json_post(&Value::Array(oversized_batch).to_string(), true)?;
    let oversized_json: Value = serde_json::from_str(&oversized_response.body)?;
    record_scenario(
        scenarios,
        "rpc_batch_over_limit_rejected_atomically",
        oversized_response.status == 200
            && oversized_json.is_object()
            && oversized_json
                .get("error")
                .and_then(|error| error.get("code"))
                .and_then(Value::as_i64)
                == Some(-32600),
        oversized_response.body,
    )?;

    let deep = format!(
        "{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockcount\",\"params\":{}0{}}}",
        "[".repeat(80),
        "]".repeat(80)
    );
    let deep_response = node.readonly.raw_json_post(&deep, true)?;
    let deep_json: Value = serde_json::from_str(&deep_response.body)?;
    record_scenario(
        scenarios,
        "rpc_excessive_json_depth_rejected",
        deep_json
            .get("error")
            .and_then(|error| error.get("code"))
            .and_then(Value::as_i64)
            == Some(-32600),
        deep_response.body,
    )?;

    // Exercise the actual HTTP/1.1 parser over a raw TCP socket. Unit tests
    // cover these branches in isolation; the live probes verify the production
    // listener, authentication ordering and response hardening together.
    let raw_get = node
        .readonly
        .raw_http_request("GET / HTTP/1.1", &[], b"", true)?;
    record_scenario(
        scenarios,
        "rpc_http_non_post_method_rejected_405",
        raw_get.status == 405,
        format!("HTTP {} body={}", raw_get.status, raw_get.body),
    )?;

    let raw_wrong_path = node.readonly.raw_http_request(
        "POST /not-rpc HTTP/1.1",
        &[("Content-Type", "application/json")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_unknown_path_rejected_404",
        raw_wrong_path.status == 404,
        format!(
            "HTTP {} body={}",
            raw_wrong_path.status, raw_wrong_path.body
        ),
    )?;

    let raw_host_rebinding = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Host", "attacker.invalid"),
            ("Content-Type", "application/json"),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_dns_rebinding_host_rejected_421",
        raw_host_rebinding.status == 421,
        format!(
            "HTTP {} body={}",
            raw_host_rebinding.status, raw_host_rebinding.body
        ),
    )?;

    let raw_text = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[("Content-Type", "text/plain")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_non_json_content_type_rejected_415",
        raw_text.status == 415,
        format!("HTTP {} body={}", raw_text.status, raw_text.body),
    )?;

    let body_len = body.len().to_string();
    let raw_duplicate_length = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Content-Length", body_len.as_str()),
            ("Content-Length", body_len.as_str()),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_duplicate_content_length_rejected_400",
        raw_duplicate_length.status == 400,
        format!(
            "HTTP {} body={}",
            raw_duplicate_length.status, raw_duplicate_length.body
        ),
    )?;

    let raw_smuggling = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Transfer-Encoding", "chunked"),
            ("Content-Length", body_len.as_str()),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_transfer_encoding_content_length_smuggling_rejected",
        raw_smuggling.status == 400,
        format!("HTTP {} body={}", raw_smuggling.status, raw_smuggling.body),
    )?;

    let raw_huge_body = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Content-Length", "999999999"),
        ],
        b"",
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_declared_huge_body_rejected_413_before_read",
        raw_huge_body.status == 413,
        format!("HTTP {} body={}", raw_huge_body.status, raw_huge_body.body),
    )?;

    let raw_wrong_auth_prebody = wrong.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Content-Length", "100000"),
        ],
        b"",
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_wrong_auth_rejected_401_before_body_read",
        raw_wrong_auth_prebody.status == 401
            && !raw_wrong_auth_prebody.body.contains(SECRET_LOG_CANARY),
        format!(
            "HTTP {} body={}",
            raw_wrong_auth_prebody.status, raw_wrong_auth_prebody.body
        ),
    )?;

    let raw_duplicate_host = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Host", "127.0.0.1"),
            ("Host", "localhost"),
            ("Content-Type", "application/json"),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_duplicate_host_rejected_400",
        raw_duplicate_host.status == 400,
        format!(
            "HTTP {} body={}",
            raw_duplicate_host.status, raw_duplicate_host.body
        ),
    )?;

    let raw_duplicate_auth = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Authorization", "Basic Zm9vOmJhcg=="),
            ("Authorization", "Basic YmF6OnF1eA=="),
        ],
        body.as_bytes(),
        false,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_duplicate_authorization_rejected_400",
        raw_duplicate_auth.status == 400,
        format!(
            "HTTP {} body={}",
            raw_duplicate_auth.status, raw_duplicate_auth.body
        ),
    )?;

    let raw_invalid_header = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Bad Header", "value"),
            ("Content-Type", "application/json"),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_invalid_header_name_rejected_400",
        raw_invalid_header.status == 400,
        format!(
            "HTTP {} body={}",
            raw_invalid_header.status, raw_invalid_header.body
        ),
    )?;

    let raw_chunked = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Transfer-Encoding", "chunked"),
        ],
        b"0\r\n\r\n",
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_unsupported_chunked_encoding_rejected_501",
        raw_chunked.status == 501,
        format!("HTTP {} body={}", raw_chunked.status, raw_chunked.body),
    )?;

    let raw_bad_length = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "application/json"),
            ("Content-Length", "12x"),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_non_numeric_content_length_rejected_400",
        raw_bad_length.status == 400,
        format!(
            "HTTP {} body={}",
            raw_bad_length.status, raw_bad_length.body
        ),
    )?;

    let long_request_line = format!("POST /{} HTTP/1.1", "x".repeat(2050));
    let raw_long_uri = node.readonly.raw_http_request(
        &long_request_line,
        &[("Content-Type", "application/json")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_overlong_uri_rejected_414",
        raw_long_uri.status == 414,
        format!("HTTP {} body={}", raw_long_uri.status, raw_long_uri.body),
    )?;

    let raw_ambiguous_line = node.readonly.raw_http_request(
        "POST  / HTTP/1.1",
        &[("Content-Type", "application/json")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_ambiguous_request_line_rejected_400",
        raw_ambiguous_line.status == 400,
        format!(
            "HTTP {} body={}",
            raw_ambiguous_line.status, raw_ambiguous_line.body
        ),
    )?;

    let raw_http2 = node.readonly.raw_http_request(
        "POST / HTTP/2.0",
        &[("Content-Type", "application/json")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_unsupported_version_rejected_400",
        raw_http2.status == 400,
        format!("HTTP {} body={}", raw_http2.status, raw_http2.body),
    )?;

    let raw_duplicate_type = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[
            ("Content-Type", "text/plain"),
            ("Content-Type", "application/json"),
        ],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_duplicate_content_type_rejected_400",
        raw_duplicate_type.status == 400,
        format!(
            "HTTP {} body={}",
            raw_duplicate_type.status, raw_duplicate_type.body
        ),
    )?;

    let raw_empty_host = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[("Host", ""), ("Content-Type", "application/json")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_empty_host_rejected_400",
        raw_empty_host.status == 400,
        format!(
            "HTTP {} body={}",
            raw_empty_host.status, raw_empty_host.body
        ),
    )?;

    let raw_folded_header = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[(" X-Folded", "value"), ("Content-Type", "application/json")],
        body.as_bytes(),
        true,
    )?;
    record_scenario(
        scenarios,
        "rpc_http_obsolete_folded_header_rejected_400",
        raw_folded_header.status == 400,
        format!(
            "HTTP {} body={}",
            raw_folded_header.status, raw_folded_header.body
        ),
    )?;

    for (name, invalid_json) in [
        ("rpc_empty_json_batch_rejected", "[]".to_owned()),
        ("rpc_scalar_json_request_rejected", "42".to_owned()),
        (
            "rpc_wrong_jsonrpc_version_rejected",
            r#"{"jsonrpc":"1.0","id":7,"method":"getblockcount","params":[]}"#.to_owned(),
        ),
        (
            "rpc_duplicate_nested_json_key_rejected",
            r#"{"jsonrpc":"2.0","id":8,"method":"getblockcount","params":{"x":1,"x":2}}"#
                .to_owned(),
        ),
    ] {
        let response = node.readonly.raw_json_post(&invalid_json, true)?;
        let parsed: Value = serde_json::from_str(&response.body)?;
        record_scenario(
            scenarios,
            name,
            response.status == 200
                && parsed
                    .get("error")
                    .and_then(|error| error.get("code"))
                    .and_then(Value::as_i64)
                    == Some(-32600),
            response.body,
        )?;
    }

    let mut complex_json =
        r#"{"jsonrpc":"2.0","id":9,"method":"getblockcount","params":["#.to_owned();
    for index in 0..50_100 {
        if index != 0 {
            complex_json.push(',');
        }
        complex_json.push('0');
    }
    complex_json.push_str("]}");
    let complex_response = node.readonly.raw_json_post(&complex_json, true)?;
    let complex_parsed: Value = serde_json::from_str(&complex_response.body)?;
    record_scenario(
        scenarios,
        "rpc_json_complexity_budget_rejects_allocation_amplification",
        complex_response.status == 200
            && complex_parsed
                .get("error")
                .and_then(|error| error.get("code"))
                .and_then(Value::as_i64)
                == Some(-32600),
        complex_response.body,
    )?;

    let duplicate_method = br#"{"jsonrpc":"2.0","id":"duplicate","method":"getblockcount","method":"stop","params":[]}"#;
    let duplicate_method_response = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[("Content-Type", "application/json")],
        duplicate_method,
        true,
    )?;
    let duplicate_method_json: Value = serde_json::from_str(&duplicate_method_response.body)?;
    record_scenario(
        scenarios,
        "rpc_duplicate_json_method_key_rejected_without_dispatch",
        duplicate_method_response.status == 200
            && duplicate_method_json
                .get("error")
                .and_then(|error| error.get("code"))
                .and_then(Value::as_i64)
                == Some(-32600),
        duplicate_method_response.body,
    )?;

    let object_id =
        br#"{"jsonrpc":"2.0","id":{"nested":true},"method":"getblockcount","params":[]}"#;
    let object_id_response = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[("Content-Type", "application/json")],
        object_id,
        true,
    )?;
    let object_id_json: Value = serde_json::from_str(&object_id_response.body)?;
    record_scenario(
        scenarios,
        "rpc_non_scalar_json_id_rejected",
        object_id_response.status == 200
            && object_id_json
                .get("error")
                .and_then(|error| error.get("code"))
                .and_then(Value::as_i64)
                == Some(-32600),
        object_id_response.body,
    )?;

    let post_corpus_health_body = json!({
        "jsonrpc": "2.0",
        "id": "post-corpus-health",
        "method": "gethealth",
        "params": [],
    })
    .to_string();
    let post_corpus_health = node.readonly.raw_http_request(
        "POST / HTTP/1.1",
        &[("Content-Type", "application/json; charset=utf-8")],
        post_corpus_health_body.as_bytes(),
        true,
    )?;
    let post_corpus_health_json: Value = serde_json::from_str(&post_corpus_health.body)?;
    let response_headers = post_corpus_health.headers.to_ascii_lowercase();
    record_scenario(
        scenarios,
        "rpc_http_parser_corpus_preserves_health_and_security_headers",
        post_corpus_health.status == 200
            && post_corpus_health_json
                .get("result")
                .and_then(|result| result.get("ok"))
                .and_then(Value::as_bool)
                == Some(true)
            && response_headers.contains("cache-control: no-store")
            && response_headers.contains("x-content-type-options: nosniff"),
        format!(
            "HTTP {} headers={} body={}",
            post_corpus_health.status,
            post_corpus_health.headers.replace("\r\n", "; "),
            post_corpus_health.body
        ),
    )?;

    run_live_rpc_connection_resilience_scenarios(cluster, scenarios)?;

    ensure!(
        node.admin
            .call("gethealth", json!([]))?
            .get("ok")
            .and_then(Value::as_bool)
            == Some(true),
        "node became unhealthy during live RPC security scenarios"
    );
    Ok(())
}
