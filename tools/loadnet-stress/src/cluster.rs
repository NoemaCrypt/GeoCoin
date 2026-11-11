//! Process lifecycle and convergence control for an isolated node cluster.

use super::*;
use std::collections::HashSet;

#[derive(Clone)]
pub(super) struct NodeRpc {
    pub(super) index: usize,
    pub(super) admin: RpcClient,
    pub(super) miner: RpcClient,
    pub(super) readonly: RpcClient,
    pub(super) p2p_port: u16,
    pub(super) rpc_port: u16,
}

pub(super) struct NodeProcess {
    pub(super) rpc: NodeRpc,
    pub(super) data_dir: PathBuf,
    pub(super) admin_user: String,
    pub(super) admin_password_file: PathBuf,
    pub(super) miner_user: String,
    pub(super) miner_password_file: PathBuf,
    pub(super) readonly_user: String,
    pub(super) readonly_password_file: PathBuf,
    pub(super) log_path: PathBuf,
    pub(super) child: Option<Child>,
}

pub(super) struct Cluster {
    pub(super) node_binary: PathBuf,
    pub(super) miner_binary: PathBuf,
    pub(super) activation_manifest: PathBuf,
    pub(super) root: PathBuf,
    pub(super) timeout: Duration,
    pub(super) mining_timeout_secs: u64,
    pub(super) nodes: Vec<NodeProcess>,
    pub(super) primary_addresses: Vec<String>,
    pub(super) blocks_mined: u64,
    pub(super) p2p_repairs: u64,
    pub(super) expected_monitor_outages: Arc<Vec<AtomicBool>>,
    pub(super) keep_alive: bool,
}

fn free_port(used: &mut HashSet<u16>) -> Result<u16> {
    for _ in 0..100 {
        let listener = TcpListener::bind(("127.0.0.1", 0))?;
        let port = listener.local_addr()?.port();
        drop(listener);
        if used.insert(port) {
            return Ok(port);
        }
    }
    bail!("failed to allocate a unique loopback port")
}

fn write_secret(path: &Path, value: &str) -> Result<()> {
    fs::write(path, format!("{value}\n"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    }
    Ok(())
}

pub(super) fn random_rpc_secret(role: &str, index: usize) -> String {
    let mut entropy = [0u8; 32];
    OsRng.fill_bytes(&mut entropy);
    let mut encoded = String::with_capacity(entropy.len() * 2);
    for byte in entropy {
        use std::fmt::Write as _;
        write!(&mut encoded, "{byte:02x}").expect("writing to a String cannot fail");
    }
    format!("load-{role}-{index}-{encoded}")
}

fn assert_private_file(path: &Path) -> Result<()> {
    ensure!(
        path.is_file(),
        "private file is missing: {}",
        path.display()
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = fs::metadata(path)?.permissions().mode() & 0o777;
        ensure!(
            mode & 0o077 == 0,
            "private file {} is accessible by group/other (mode {mode:o})",
            path.display()
        );
    }
    Ok(())
}

impl Cluster {
    pub(super) fn start(args: &Args) -> Result<Self> {
        let node_binary = fs::canonicalize(&args.node_binary)?;
        let miner_binary = fs::canonicalize(&args.miner_binary)?;
        let activation_manifest = fs::canonicalize(&args.activation_manifest)?;
        let root = fs::canonicalize(&args.work_dir)?;
        let timeout = Duration::from_secs(args.startup_timeout_secs);
        let mut used_ports = HashSet::new();
        let mut nodes = Vec::with_capacity(args.nodes);
        let expected_monitor_outages = Arc::new(
            (0..args.nodes)
                .map(|_| AtomicBool::new(false))
                .collect::<Vec<_>>(),
        );

        for index in 0..args.nodes {
            let node_root = root.join(format!("node-{index}"));
            fs::create_dir_all(&node_root)?;
            let data_dir = node_root.join("data");
            fs::create_dir_all(&data_dir)?;
            let admin_user = format!("load-admin-{index}");
            let admin_password = random_rpc_secret("admin", index);
            let miner_user = format!("load-miner-{index}");
            let miner_password = random_rpc_secret("miner", index);
            let readonly_user = format!("load-readonly-{index}");
            let readonly_password = random_rpc_secret("readonly", index);
            let admin_password_file = node_root.join("rpc-admin.pass");
            let miner_password_file = node_root.join("rpc-miner.pass");
            let readonly_password_file = node_root.join("rpc-readonly.pass");
            write_secret(&admin_password_file, &admin_password)?;
            write_secret(&miner_password_file, &miner_password)?;
            write_secret(&readonly_password_file, &readonly_password)?;
            let p2p_port = free_port(&mut used_ports)?;
            let rpc_port = free_port(&mut used_ports)?;
            let url = format!("http://127.0.0.1:{rpc_port}/");
            let admin = RpcClient::new(
                url.clone(),
                admin_user.clone(),
                admin_password.clone(),
                Duration::from_secs(30),
            )?;
            let miner = RpcClient::new(
                url.clone(),
                miner_user.clone(),
                miner_password.clone(),
                Duration::from_secs(30),
            )?;
            let readonly = RpcClient::new(
                url,
                readonly_user.clone(),
                readonly_password.clone(),
                Duration::from_secs(30),
            )?;
            nodes.push(NodeProcess {
                rpc: NodeRpc {
                    index,
                    admin,
                    miner,
                    readonly,
                    p2p_port,
                    rpc_port,
                },
                data_dir,
                admin_user,
                admin_password_file,
                miner_user,
                miner_password_file,
                readonly_user,
                readonly_password_file,
                log_path: node_root.join("node.log"),
                child: None,
            });
        }

        let mut cluster = Self {
            node_binary,
            miner_binary,
            activation_manifest,
            root,
            timeout,
            mining_timeout_secs: args.mining_timeout_secs,
            nodes,
            primary_addresses: Vec::new(),
            blocks_mined: 0,
            p2p_repairs: 0,
            expected_monitor_outages,
            keep_alive: false,
        };
        for index in 0..cluster.nodes.len() {
            cluster.launch_node(index)?;
        }
        for index in 0..cluster.nodes.len() {
            cluster.wait_rpc(index)?;
        }
        Ok(cluster)
    }

    pub(super) fn rpc_nodes(&self) -> Arc<Vec<NodeRpc>> {
        Arc::new(self.nodes.iter().map(|node| node.rpc.clone()).collect())
    }

    pub(super) fn launch_node(&mut self, index: usize) -> Result<()> {
        let peer_endpoints = self
            .nodes
            .iter()
            .enumerate()
            // Exactly one persistent dialer per pair.  Making both loopback
            // nodes dial each other can repeatedly race authenticated
            // identity de-duplication after a restart and eventually trip the
            // per-IP inbound limiter, creating an artificial localhost-only
            // partition.  Higher indices dial lower indices, which still
            // yields a complete mesh with one transport per pair.
            .filter(|(peer_index, _)| *peer_index < index)
            .map(|(_, peer)| format!("127.0.0.1:{}", peer.rpc.p2p_port))
            .collect::<Vec<_>>();
        let node = &mut self.nodes[index];
        let stdout = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&node.log_path)?;
        let stderr = stdout.try_clone()?;
        let mut command = Command::new(&self.node_binary);
        command
            .arg("--data-dir")
            .arg(&node.data_dir)
            .arg("--p2p-port")
            .arg(node.rpc.p2p_port.to_string())
            .arg("--rpc-port")
            .arg(node.rpc.rpc_port.to_string())
            .arg("--rpc")
            .arg("--rpc-user")
            .arg(&node.admin_user)
            .arg("--rpc-pass-file")
            .arg(&node.admin_password_file)
            .arg("--rpc-miner-user")
            .arg(&node.miner_user)
            .arg("--rpc-miner-pass-file")
            .arg(&node.miner_password_file)
            .arg("--rpc-readonly-user")
            .arg(&node.readonly_user)
            .arg("--rpc-readonly-pass-file")
            .arg(&node.readonly_password_file)
            .arg("--network")
            .arg(NETWORK)
            .arg("--activation-manifest")
            .arg(&self.activation_manifest)
            .arg("--archive")
            .arg("--status-interval")
            .arg("3600")
            .env("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "1")
            .env("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "0")
            .stdout(Stdio::from(stdout))
            .stderr(Stdio::from(stderr))
            .current_dir(&self.root);
        for endpoint in peer_endpoints {
            command.arg("--connect").arg(endpoint);
        }
        node.child = Some(command.spawn().with_context(|| {
            format!(
                "cannot start node {index} from {}",
                self.node_binary.display()
            )
        })?);
        Ok(())
    }

    pub(super) fn wait_rpc(&mut self, index: usize) -> Result<()> {
        let deadline = Instant::now() + self.timeout;
        let mut last_error = String::new();
        while Instant::now() < deadline {
            if let Some(status) = self.nodes[index]
                .child
                .as_mut()
                .and_then(|child| child.try_wait().ok())
                .flatten()
            {
                bail!(
                    "node {index} exited during startup with {status}; log={}",
                    self.nodes[index].log_path.display()
                );
            }
            match self.nodes[index].rpc.admin.call("getblockcount", json!([])) {
                Ok(_) => return Ok(()),
                Err(error) => last_error = error.to_string(),
            }
            thread::sleep(Duration::from_millis(250));
        }
        bail!(
            "node {index} RPC did not start: {last_error}; log={}",
            self.nodes[index].log_path.display()
        )
    }

    pub(super) fn create_wallets(&mut self) -> Result<()> {
        self.primary_addresses.clear();
        for node in &self.nodes {
            let created = node.rpc.admin.call("createwallet", json!([]))?;
            let address = created
                .get("address")
                .and_then(Value::as_str)
                .ok_or_else(|| {
                    anyhow!("node {} createwallet returned no address", node.rpc.index)
                })?;
            self.primary_addresses.push(address.to_owned());
        }
        Ok(())
    }

    pub(super) fn connect_full_mesh(&self) -> Result<()> {
        // Every process starts with every other loopback endpoint as a
        // persistent --connect target. Wait for authenticated READY sessions,
        // not merely for transient TCP sockets counted during handshakes.
        let deadline = Instant::now() + self.timeout;
        let required_peers = (self.nodes.len() - 1) as u64;
        let mut stable_samples = 0;
        let mut next_nudge = Instant::now();
        while Instant::now() < deadline {
            if Instant::now() >= next_nudge {
                // A node that tried a follower while it was intentionally down
                // can still be in exponential backoff. One directed onetry per
                // pair bypasses that wait; authenticated identity de-duplication
                // deterministically retains only one transport.
                for source in 1..self.nodes.len() {
                    for target in 0..source {
                        let endpoint = format!("127.0.0.1:{}", self.nodes[target].rpc.p2p_port);
                        let _ = self.nodes[source]
                            .rpc
                            .admin
                            .call("addnode", json!([endpoint, "onetry"]));
                    }
                }
                next_nudge = Instant::now() + Duration::from_secs(5);
            }
            let connected = self
                .nodes
                .iter()
                .filter(|node| {
                    node.rpc
                        .admin
                        .call("getconnectiondiagnostics", json!([]))
                        .ok()
                        .and_then(|value| {
                            value
                                .get("stats")
                                .and_then(|stats| stats.get("authenticated_peers"))
                                .and_then(Value::as_u64)
                        })
                        .unwrap_or(0)
                        >= required_peers
                })
                .count();
            if connected == self.nodes.len() {
                stable_samples += 1;
                if stable_samples >= 3 {
                    return Ok(());
                }
            } else {
                stable_samples = 0;
            }
            thread::sleep(Duration::from_millis(300));
        }
        let counts = self
            .nodes
            .iter()
            .map(|node| {
                node.rpc
                    .admin
                    .call("getconnectiondiagnostics", json!([]))
                    .ok()
                    .and_then(|value| {
                        value
                            .get("stats")
                            .and_then(|stats| stats.get("authenticated_peers"))
                            .and_then(Value::as_u64)
                    })
                    .unwrap_or(0)
            })
            .collect::<Vec<_>>();
        bail!(
            "loadnet did not establish a full mesh (need {required_peers} peers per node, got {counts:?})"
        )
    }

    pub(super) fn verify_genesis_and_foundation(&self) -> Result<()> {
        for node in &self.nodes {
            let hash: String = node.rpc.admin.call_as("getblockhash", json!([0]))?;
            ensure!(
                hash == GENESIS_HASH,
                "node {} has wrong genesis {hash}",
                node.rpc.index
            );
            let block = node.rpc.admin.call("getblockbyheight", json!([0]))?;
            let coinbase_txid = block
                .get("tx")
                .and_then(Value::as_array)
                .and_then(|txids| txids.first())
                .and_then(Value::as_str)
                .ok_or_else(|| anyhow!("node {} genesis has no coinbase txid", node.rpc.index))?;
            let coinbase = node
                .rpc
                .admin
                .call("getrawtransaction", json!([coinbase_txid, 1]))?;
            let encoded = coinbase.to_string();
            ensure!(
                encoded.contains(FOUNDATION_MULTISIG),
                "node {} genesis does not pay the loadnet foundation",
                node.rpc.index
            );
        }
        Ok(())
    }

    pub(super) fn assert_secret_file_permissions(&self) -> Result<()> {
        for node in &self.nodes {
            assert_private_file(&node.admin_password_file)?;
            assert_private_file(&node.miner_password_file)?;
            assert_private_file(&node.readonly_password_file)?;
            assert_private_file(&node.data_dir.join("node.key"))?;
            assert_private_file(&node.data_dir.join("wallet.dat"))?;
            let wallet_backup = node.data_dir.join("wallet.dat.bak");
            if wallet_backup.exists() {
                assert_private_file(&wallet_backup)?;
            }
        }
        Ok(())
    }

    pub(super) fn generate_accounts(&self, count: usize) -> Result<Vec<Account>> {
        let next = AtomicUsize::new(0);
        let accounts = Mutex::new(Vec::with_capacity(count));
        thread::scope(|scope| {
            for worker in 0..self.nodes.len().min(8) {
                let next = &next;
                let accounts = &accounts;
                scope.spawn(move || {
                    loop {
                        let id = next.fetch_add(1, Ordering::Relaxed);
                        if id >= count {
                            break;
                        }
                        let owner_node = id % self.nodes.len();
                        let result: Result<Account> = (|| {
                            let address: String = self.nodes[owner_node].rpc.admin.call_as(
                                "getnewaddress",
                                json!([format!("load-account-{id:04}")]),
                            )?;
                            Ok(Account {
                                id,
                                owner_node,
                                address,
                            })
                        })();
                        match result {
                            Ok(account) => accounts
                                .lock()
                                .expect("account mutex poisoned")
                                .push(account),
                            Err(error) => {
                                eprintln!("[loadnet] address worker {worker}: {error:#}");
                                break;
                            }
                        }
                    }
                });
            }
        });
        let mut accounts = accounts
            .into_inner()
            .map_err(|_| anyhow!("account mutex poisoned"))?;
        accounts.sort_by_key(|account| account.id);
        ensure!(
            accounts.len() == count,
            "created {}/{} accounts",
            accounts.len(),
            count
        );
        let unique: HashSet<&str> = accounts
            .iter()
            .map(|account| account.address.as_str())
            .collect();
        ensure!(
            unique.len() == count,
            "generated duplicate account addresses"
        );
        Ok(accounts)
    }

    pub(super) fn height(&self, index: usize) -> Result<u64> {
        Ok(self.nodes[index]
            .rpc
            .admin
            .call_as("getblockcount", json!([]))?)
    }

    pub(super) fn mine_blocks(&mut self, blocks: u64) -> Result<()> {
        self.mine_blocks_from(0, blocks)
    }

    pub(super) fn mine_blocks_from(&mut self, node_index: usize, blocks: u64) -> Result<()> {
        if blocks == 0 {
            return Ok(());
        }
        let (before, target) = self.run_miner(node_index, blocks)?;
        if self
            .wait_converged_from(node_index, target, Duration::from_secs(8))
            .is_err()
        {
            self.p2p_repairs += 1;
            self.repair_block_range(before + 1, target)?;
        }
        self.ensure_converged_from(node_index, target)
    }

    pub(super) fn run_miner(&mut self, node_index: usize, blocks: u64) -> Result<(u64, u64)> {
        ensure!(blocks > 0, "run_miner requires at least one block");
        let before = self.height(node_index)?;
        let tip = self.nodes[node_index]
            .rpc
            .admin
            .call("getblockbyheight", json!([before]))?;
        let tip_time = tip
            .get("time")
            .and_then(Value::as_u64)
            .ok_or_else(|| anyhow!("tip block has no time"))?;
        let output = Command::new(&self.miner_binary)
            .arg("--url")
            .arg(format!(
                "http://127.0.0.1:{}/",
                self.nodes[node_index].rpc.rpc_port
            ))
            .arg("--rpc-user")
            .arg(&self.nodes[node_index].miner_user)
            .arg("--rpc-pass-file")
            .arg(&self.nodes[node_index].miner_password_file)
            .arg("--address")
            .arg(&self.primary_addresses[node_index])
            .arg("--blocks")
            .arg(blocks.to_string())
            .arg("--threads")
            .arg("1")
            .arg("--timeout")
            .arg(self.mining_timeout_secs.to_string())
            .arg("--timestamp")
            .arg((tip_time + 1).to_string())
            .arg("--timestamp-step")
            .arg("1")
            .output()
            .with_context(|| format!("failed to run {}", self.miner_binary.display()))?;
        ensure!(
            output.status.success(),
            "miner failed: stdout={} stderr={}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        self.blocks_mined += blocks;
        let target = before + blocks;
        ensure!(
            self.height(node_index)? == target,
            "miner node {node_index} did not reach target height {target}"
        );
        Ok((before, target))
    }

    pub(super) fn wait_subset_converged(
        &self,
        source: usize,
        target: u64,
        members: &[usize],
        timeout: Duration,
    ) -> Result<()> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Ok(expected) = self.nodes[source]
                .rpc
                .admin
                .call_as::<String>("getblockhash", json!([target]))
            {
                let all_match = members.iter().all(|index| {
                    self.nodes[*index]
                        .rpc
                        .admin
                        .call_as::<u64>("getblockcount", json!([]))
                        .ok()
                        .is_some_and(|height| height == target)
                        && self.nodes[*index]
                            .rpc
                            .admin
                            .call_as::<String>("getblockhash", json!([target]))
                            .ok()
                            .as_deref()
                            == Some(expected.as_str())
                });
                if all_match {
                    return Ok(());
                }
            }
            thread::sleep(Duration::from_millis(200));
        }
        bail!("partition {members:?} did not converge at height {target}")
    }

    pub(super) fn ensure_subset_connected(&self, members: &[usize]) -> Result<()> {
        ensure!(!members.is_empty(), "peer subset must not be empty");
        if members.len() == 1 {
            return Ok(());
        }

        let required_peers = (members.len() - 1) as u64;
        let deadline = Instant::now() + self.timeout;
        let mut stable_samples = 0;
        let mut next_nudge = Instant::now();
        while Instant::now() < deadline {
            let connected = members
                .iter()
                .filter(|index| {
                    self.nodes[**index]
                        .rpc
                        .admin
                        .call("getconnectiondiagnostics", json!([]))
                        .ok()
                        .and_then(|value| {
                            value
                                .get("stats")
                                .and_then(|stats| stats.get("authenticated_peers"))
                                .and_then(Value::as_u64)
                        })
                        .unwrap_or(0)
                        >= required_peers
                })
                .count();
            if connected == members.len() {
                stable_samples += 1;
                if stable_samples >= 3 {
                    return Ok(());
                }
            } else {
                stable_samples = 0;
                if Instant::now() >= next_nudge {
                    // Keep a single deterministic dialer for each pair.  The
                    // ten-second retry interval also remains safely below the
                    // localhost per-IP connection limiter during partitions.
                    for source_position in 1..members.len() {
                        let source = members[source_position];
                        for &target in members.iter().take(source_position) {
                            let endpoint = format!("127.0.0.1:{}", self.nodes[target].rpc.p2p_port);
                            let _ = self.nodes[source]
                                .rpc
                                .admin
                                .call("addnode", json!([endpoint, "onetry"]));
                        }
                    }
                    next_nudge = Instant::now() + Duration::from_secs(10);
                }
            }
            thread::sleep(Duration::from_millis(300));
        }
        bail!("partition {members:?} did not establish authenticated connectivity")
    }

    pub(super) fn mine_partition_blocks(
        &mut self,
        miner: usize,
        blocks: u64,
        members: &[usize],
    ) -> Result<(u64, u64)> {
        ensure!(members.contains(&miner), "partition does not contain miner");
        self.ensure_subset_connected(members)?;
        let range = self.run_miner(miner, blocks)?;
        self.wait_subset_converged(miner, range.1, members, self.timeout)?;
        Ok(range)
    }

    pub(super) fn wait_converged(&self, target: u64, timeout: Duration) -> Result<()> {
        self.wait_converged_from(0, target, timeout)
    }

    pub(super) fn wait_converged_from(
        &self,
        source: usize,
        target: u64,
        timeout: Duration,
    ) -> Result<()> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            let expected = self.nodes[source]
                .rpc
                .admin
                .call_as::<String>("getblockhash", json!([target]));
            if let Ok(expected) = expected {
                let all_match = self.nodes.iter().all(|node| {
                    node.rpc
                        .admin
                        .call_as::<u64>("getblockcount", json!([]))
                        .ok()
                        .is_some_and(|height| height >= target)
                        && node
                            .rpc
                            .admin
                            .call_as::<String>("getblockhash", json!([target]))
                            .ok()
                            .as_deref()
                            == Some(expected.as_str())
                });
                if all_match {
                    return Ok(());
                }
            }
            thread::sleep(Duration::from_millis(250));
        }
        bail!("nodes did not converge at height {target}")
    }

    pub(super) fn ensure_converged(&self, target: u64) -> Result<()> {
        self.wait_converged(target, self.timeout)
    }

    pub(super) fn ensure_converged_from(&self, source: usize, target: u64) -> Result<()> {
        self.wait_converged_from(source, target, self.timeout)
    }

    pub(super) fn repair_block_range(&self, first: u64, last: u64) -> Result<()> {
        for height in first..=last {
            let expected: String = self.nodes[0]
                .rpc
                .admin
                .call_as("getblockhash", json!([height]))?;
            let raw: String = self.nodes[0]
                .rpc
                .admin
                .call_as("getblockbyheight", json!([height, false]))?;
            for node in self.nodes.iter().skip(1) {
                let already = node
                    .rpc
                    .admin
                    .call_as::<u64>("getblockcount", json!([]))
                    .ok()
                    .is_some_and(|tip| tip >= height)
                    && node
                        .rpc
                        .admin
                        .call_as::<String>("getblockhash", json!([height]))
                        .ok()
                        .as_deref()
                        == Some(expected.as_str());
                if already {
                    continue;
                }
                match node.rpc.miner.call("submitblock", json!([raw])) {
                    Ok(_) => {}
                    Err(error) => {
                        let now_matches = node
                            .rpc
                            .admin
                            .call_as::<u64>("getblockcount", json!([]))
                            .ok()
                            .is_some_and(|tip| tip >= height)
                            && node
                                .rpc
                                .admin
                                .call_as::<String>("getblockhash", json!([height]))
                                .ok()
                                .as_deref()
                                == Some(expected.as_str());
                        ensure!(
                            now_matches,
                            "node {} rejected repair block {height}: {error}",
                            node.rpc.index
                        );
                    }
                }
            }
        }
        Ok(())
    }

    pub(super) fn assert_fixed_difficulty(&self) -> Result<()> {
        let height = self.height(0)?;
        for sample_height in [1, height / 2, height] {
            let block = self.nodes[0]
                .rpc
                .admin
                .call("getblockbyheight", json!([sample_height]))?;
            let bits_match = block.get("bits").and_then(Value::as_str) == Some("207fffff")
                || block.get("bits").and_then(Value::as_u64) == Some(0x207f_ffff);
            let encoded = block.to_string().to_lowercase();
            ensure!(
                bits_match || encoded.contains("207fffff"),
                "height {sample_height} does not use fixed bits 207fffff: {block}"
            );
        }
        Ok(())
    }

    pub(super) fn fund_accounts(
        &self,
        accounts: &[Account],
        amount: u64,
        fee: u64,
    ) -> Result<Vec<String>> {
        let miner_address = &self.primary_addresses[0];
        let height = self.height(0)?;
        let utxos = self.nodes[0]
            .rpc
            .admin
            .call("getaddressutxos", json!([miner_address, 0, 5000]))?;
        let mut mature = utxos
            .as_array()
            .ok_or_else(|| anyhow!("getaddressutxos did not return an array"))?
            .iter()
            .filter(|utxo| {
                utxo.get("coinbase").and_then(Value::as_bool) == Some(true)
                    && utxo
                        .get("height")
                        .and_then(Value::as_u64)
                        .is_some_and(|created| height + 1 >= created + COINBASE_MATURITY)
            })
            .cloned()
            .collect::<Vec<_>>();
        mature.sort_by_key(|utxo| {
            utxo.get("height")
                .and_then(Value::as_u64)
                .unwrap_or(u64::MAX)
        });
        ensure!(
            mature.len() >= 4,
            "only {} mature miner coinbases available",
            mature.len()
        );

        let mut txids = Vec::new();
        for (batch_index, batch) in accounts.chunks(accounts.len().div_ceil(4)).enumerate() {
            ensure!(
                batch_index < 4,
                "funding unexpectedly needs more than four fan-outs"
            );
            let utxo = &mature[batch_index];
            let input_value = utxo
                .get("value")
                .and_then(Value::as_u64)
                .ok_or_else(|| anyhow!("coinbase UTXO lacks exact value"))?;
            let total = amount
                .checked_mul(batch.len() as u64)
                .ok_or_else(|| anyhow!("fan-out total overflow"))?;
            ensure!(
                input_value > total + fee + DUST_ANGLES,
                "coinbase {batch_index} cannot cover {} outputs",
                batch.len()
            );
            let mut outputs = batch
                .iter()
                .map(|account| json!({"address": account.address, "value_angles": amount}))
                .collect::<Vec<_>>();
            outputs.push(json!({
                "address": miner_address,
                "value_angles": input_value - total - fee,
            }));
            let inputs = json!([{
                "txid": utxo.get("txid").and_then(Value::as_str).unwrap_or_default(),
                "vout": utxo.get("vout").and_then(Value::as_u64).unwrap_or(0),
            }]);
            let raw: String = self.nodes[0]
                .rpc
                .admin
                .call_as("createrawtransaction", json!([inputs, outputs]))?;
            let signed = sign_raw(&self.nodes[0].rpc.admin, &raw)?;
            preflight_allowed(&self.nodes[0].rpc.admin, &signed.hex)?;
            broadcast_signed(&self.rpc_nodes(), &signed)?;
            txids.push(signed.txid);
        }
        ensure!(
            txids.len() == 4,
            "expected four funding fan-outs, got {}",
            txids.len()
        );
        Ok(txids)
    }

    pub(super) fn verify_all_accounts_funded(
        &self,
        accounts: &[Account],
        minimum: u64,
    ) -> Result<()> {
        let failures = Mutex::new(Vec::new());
        thread::scope(|scope| {
            for shard in 0..self.nodes.len() {
                let failures = &failures;
                scope.spawn(move || {
                    for account in accounts
                        .iter()
                        .filter(|account| account.id % self.nodes.len() == shard)
                    {
                        let mut last = String::from("no attempt");
                        let mut funded = false;
                        // Prefer followers first: the primary miner is restarted
                        // during live features and its UTXO RPC can lag.
                        let order = (1..self.nodes.len()).chain(0..1);
                        for attempt in 0_u32..6 {
                            for index in order.clone() {
                                match self.nodes[index].rpc.admin.call(
                                    "getaddressutxos",
                                    json!([&account.address, 0, 5000]),
                                ) {
                                    Ok(value) => {
                                        let sum = value
                                            .as_array()
                                            .map(|items| {
                                                items
                                                    .iter()
                                                    .map(|utxo| {
                                                        utxo.get("value")
                                                            .and_then(Value::as_u64)
                                                            .unwrap_or(0)
                                                    })
                                                    .sum::<u64>()
                                            })
                                            .unwrap_or(0);
                                        if sum >= minimum {
                                            funded = true;
                                            break;
                                        }
                                        last = format!(
                                            "node{index} sum={sum} count={}",
                                            value.as_array().map(Vec::len).unwrap_or(0)
                                        );
                                    }
                                    Err(error) => {
                                        last = format!("node{index} rpc {error}");
                                    }
                                }
                            }
                            if funded {
                                break;
                            }
                            thread::sleep(Duration::from_millis(50 * u64::from(attempt + 1)));
                        }
                        if !funded {
                            failures.lock().expect("funding mutex poisoned").push(
                                format!(
                                    "id={} owner={} {} {last}",
                                    account.id, account.owner_node, account.address
                                ),
                            );
                        }
                    }
                });
            }
        });
        let failures = failures
            .into_inner()
            .map_err(|_| anyhow!("funding mutex poisoned"))?;
        ensure!(
            failures.is_empty(),
            "{} accounts are not funded on the cluster: {}",
            failures.len(),
            failures.iter().take(12).cloned().collect::<Vec<_>>().join(" | ")
        );
        Ok(())
    }

    pub(super) fn mine_until_mempools_empty(&mut self, max_blocks: u64) -> Result<()> {
        for _ in 0..max_blocks {
            let sizes = self.mempool_sizes()?;
            if sizes.iter().all(|size| *size == 0) {
                return Ok(());
            }
            self.mine_blocks(1)?;
        }
        bail!(
            "mempools remain non-empty after {max_blocks} blocks: {:?}",
            self.mempool_sizes()?
        )
    }

    pub(super) fn raw_mempool(&self, index: usize) -> Result<Vec<String>> {
        let value = self.nodes[index]
            .rpc
            .readonly
            .call("getrawmempool", json!([]))?;
        value
            .as_array()
            .ok_or_else(|| anyhow!("node {index} getrawmempool returned a non-array"))?
            .iter()
            .map(|item| {
                item.as_str()
                    .map(ToOwned::to_owned)
                    .ok_or_else(|| anyhow!("node {index} mempool entry is not a txid"))
            })
            .collect()
    }

    pub(super) fn mempool_contains(&self, index: usize, txid: &str) -> Result<bool> {
        Ok(self.raw_mempool(index)?.iter().any(|item| item == txid))
    }

    pub(super) fn active_chain_txids_from(
        &self,
        index: usize,
        from_height: u64,
    ) -> Result<HashSet<String>> {
        let tip = self.height(index)?;
        let mut found = HashSet::new();
        for height in from_height..=tip {
            let block = self.nodes[index]
                .rpc
                .readonly
                .call("getblockbyheight", json!([height]))?;
            let Some(txids) = block.get("tx").and_then(Value::as_array) else {
                continue;
            };
            for txid in txids {
                if let Some(txid) = txid.as_str() {
                    found.insert(txid.to_owned());
                }
            }
        }
        Ok(found)
    }

    pub(super) fn mempool_and_template_diagnostics(&self, txid: &str) -> Result<String> {
        let mut parts = Vec::new();
        for node in &self.nodes {
            let mempool = node.rpc.readonly.call("getrawmempool", json!([]))?;
            let info = node.rpc.miner.call("getmempoolinfo", json!([]))?;
            parts.push(format!(
                "node{} height={} mempool={} info={}",
                node.rpc.index,
                self.height(node.rpc.index).unwrap_or(u64::MAX),
                mempool,
                info
            ));
        }
        let template = self.nodes[0].rpc.miner.call(
            "getblocktemplate",
            json!([{ "address": self.primary_addresses[0] }]),
        );
        match template {
            Ok(value) => {
                let txids = value
                    .get("transactions")
                    .and_then(Value::as_array)
                    .map(|items| {
                        items
                            .iter()
                            .filter_map(|item| item.get("txid").and_then(Value::as_str))
                            .collect::<Vec<_>>()
                    })
                    .unwrap_or_default();
                parts.push(format!(
                    "template_height={} template_contains_{txid}={} template_txids={txids:?}",
                    value.get("height").unwrap_or(&Value::Null),
                    txids.iter().any(|item| *item == txid)
                ));
            }
            Err(error) => parts.push(format!("getblocktemplate failed: {error}")),
        }
        Ok(parts.join(" | "))
    }

    pub(super) fn mine_until_txids_in_active_chain(
        &mut self,
        txids: &[String],
        since_height: u64,
        max_blocks: u64,
    ) -> Result<()> {
        if txids.is_empty() {
            return self.mine_until_mempools_empty(max_blocks);
        }
        for mined in 0..=max_blocks {
            let found = self.active_chain_txids_from(0, since_height)?;
            if txids.iter().all(|txid| found.contains(txid)) {
                let tip = self.height(0)?;
                self.ensure_converged(tip)?;
                let replicated = (0..self.nodes.len()).all(|index| {
                    self.active_chain_txids_from(index, since_height)
                        .ok()
                        .is_some_and(|set| txids.iter().all(|txid| set.contains(txid)))
                });
                ensure!(
                    replicated,
                    "txids are in node-0's active chain at height {tip} but were not replicated: {}",
                    self.mempool_and_template_diagnostics(&txids[0])?
                );
                return Ok(());
            }
            if mined == max_blocks {
                break;
            }
            let still_pending = txids.iter().any(|txid| {
                (0..self.nodes.len()).any(|index| self.mempool_contains(index, txid).unwrap_or(false))
            });
            if !still_pending {
                let missing = txids
                    .iter()
                    .filter(|txid| !found.contains(*txid))
                    .cloned()
                    .collect::<Vec<_>>();
                bail!(
                    "txids left mempool without entering the active chain since height {since_height}: missing={missing:?} {}",
                    self.mempool_and_template_diagnostics(&missing[0])?
                );
            }
            self.mine_blocks(1)?;
        }
        let found = self.active_chain_txids_from(0, since_height)?;
        let missing = txids
            .iter()
            .filter(|txid| !found.contains(*txid))
            .cloned()
            .collect::<Vec<_>>();
        bail!(
            "txids were not in the active chain after {max_blocks} blocks since height {since_height}: missing={missing:?} {}",
            self.mempool_and_template_diagnostics(
                missing.first().map(String::as_str).unwrap_or("")
            )?
        )
    }

    pub(super) fn mempool_sizes(&self) -> Result<Vec<u64>> {
        self.nodes
            .iter()
            .map(|node| {
                let info = node.rpc.miner.call("getmempoolinfo", json!([]))?;
                info.get("size")
                    .and_then(Value::as_u64)
                    .ok_or_else(|| anyhow!("node {} mempool info lacks size", node.rpc.index))
            })
            .collect()
    }

    pub(super) fn assert_all_mempools_empty(&self) -> Result<()> {
        let sizes = self.mempool_sizes()?;
        ensure!(
            sizes.iter().all(|size| *size == 0),
            "final mempools are not empty: {sizes:?}"
        );
        Ok(())
    }

    pub(super) fn stop_node(&mut self, index: usize, graceful: bool) -> Result<()> {
        ensure!(
            index < self.nodes.len(),
            "node index {index} is out of range"
        );
        self.expected_monitor_outages[index].store(true, Ordering::Release);
        if graceful {
            let _ = self.nodes[index].rpc.admin.call("stop", json!([]));
        } else if let Some(child) = self.nodes[index].child.as_mut()
            && child.try_wait()?.is_none()
        {
            child.kill()?;
        }
        let deadline = Instant::now() + Duration::from_secs(15);
        while Instant::now() < deadline {
            if self.nodes[index]
                .child
                .as_mut()
                .and_then(|child| child.try_wait().ok())
                .flatten()
                .is_some()
            {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        if let Some(child) = self.nodes[index].child.as_mut()
            && child.try_wait()?.is_none()
        {
            child.kill()?;
            child.wait()?;
        }
        self.nodes[index].child = None;
        Ok(())
    }

    pub(super) fn restart_node(&mut self, index: usize) -> Result<()> {
        self.stop_node(index, true)?;
        self.start_stopped_node(index)
    }

    pub(super) fn start_stopped_node(&mut self, index: usize) -> Result<()> {
        ensure!(
            self.nodes[index].child.is_none(),
            "node {index} is already running"
        );
        self.launch_node(index)?;
        self.wait_rpc(index)?;
        self.expected_monitor_outages[index].store(false, Ordering::Release);
        Ok(())
    }

    pub(super) fn crash_restart_node(&mut self, index: usize) -> Result<()> {
        self.stop_node(index, false)?;
        self.start_stopped_node(index)
    }

    pub(super) fn assert_processes_alive(&mut self) -> Result<()> {
        for node in &mut self.nodes {
            let status = node
                .child
                .as_mut()
                .and_then(|child| child.try_wait().ok())
                .flatten();
            ensure!(
                status.is_none(),
                "node {} exited unexpectedly with {status:?}",
                node.rpc.index
            );
        }
        Ok(())
    }

    pub(super) fn collect_final_diagnostics(&self) -> Result<Vec<Value>> {
        self.nodes
            .iter()
            .map(|node| {
                let chain = node.rpc.admin.call("getblockchaininfo", json!([]))?;
                let health = node.rpc.admin.call("gethealth", json!([]))?;
                let mempool = node.rpc.miner.call("getmempoolinfo", json!([]))?;
                let peers = node.rpc.admin.call("getconnectiondiagnostics", json!([]))?;
                Ok(json!({
                    "node": node.rpc.index,
                    "chain": chain,
                    "health": health,
                    "mempool": mempool,
                    "connections": peers,
                }))
            })
            .collect()
    }

    pub(super) fn scan_logs(&self) -> Result<Vec<LogFinding>> {
        let mut findings = Vec::new();
        for node in &self.nodes {
            let log = fs::read_to_string(&node.log_path)
                .with_context(|| format!("cannot read {}", node.log_path.display()))?;
            for (line_index, message) in log.lines().enumerate() {
                let level = if message.contains("[FATAL]") {
                    "FATAL"
                } else if message.contains("[ERROR]") {
                    "ERROR"
                } else if message.contains("[WARN]") {
                    "WARN"
                } else {
                    continue;
                };
                let expected = is_expected_log_finding(level, message);
                findings.push(LogFinding {
                    node: node.rpc.index,
                    line: line_index + 1,
                    level: level.to_owned(),
                    expected,
                    message: message.to_owned(),
                });
            }
        }
        Ok(findings)
    }

    pub(super) fn logs_contain(&self, needle: &str) -> Result<bool> {
        for node in &self.nodes {
            let log = fs::read_to_string(&node.log_path)
                .with_context(|| format!("cannot read {}", node.log_path.display()))?;
            if log.contains(needle) {
                return Ok(true);
            }
        }
        Ok(false)
    }
}

pub(super) fn is_expected_log_finding(level: &str, message: &str) -> bool {
    match level {
        "ERROR" => {
            message.contains("create_transaction_with_change: No UTXOs selected")
                || message.contains("verify_input: verify_transaction_input failed")
                || message.contains("NFT tags must be unique")
                || message.contains("Polygon edges intersect")
                || message.contains("Invalid content_hash")
                || message.contains("Polygon must have 3..MAX points")
                || message.contains("Royalty percent exceeds 100%")
                || message.contains("Official cadastral area mismatch")
                || message.contains("Duplicate cadastral_number")
                || message.contains("Polygon intersects existing cadastral plot")
                || message.contains("Polygon area is zero")
                || message.contains("Invalid polygon coordinates")
                || message.contains("Owner name is required")
                || message.contains("Cadastral number is required")
                || message.contains("Official cadastral area must be > 0")
                || message.contains("Projected area is zero")
                || message.contains("NFT does not exist")
                || message.contains("Sender is not the owner")
                || message.contains("Invalid NFT transfer signature")
        }
        "WARN" => {
            [
                "Slow block processing:",
                "Ignoring BLOCK response",
                "Peer transport send failed",
                "Peer transport send incomplete",
                "Peer transport receive failed (error=104)",
                "[PeerManager::broadcast] Send FAILED",
                "RPCException code=-8",
                "RPCException code=-4",
                "RPCException code=-5",
                "RPCException code=-6",
                "RPCException code=-25",
                "RPCException code=-26",
                "RPCException code=-29",
                "RPCException code=-32602",
                "UTXO snapshot count mismatch",
                "NFT creation validation",
                "NFT transfer validation",
                "NFT burn validation",
                "rpc_authz_denied method=",
                "rpc_auth_failed_pre_body user=",
                "rpc_host_rejected client=127.0.0.1",
                "Auth failed (user=wrong-user)",
                "Rejected RPC JSON payload with code -32700",
                "Rejected RPC JSON payload with code -32600",
                "Peer temporarily banned: 127.0.0.1",
                "Outbound peer is temporarily banned: 127.0.0.1:",
                "Rate limit exceeded by 127.0.0.1",
                "Inbound from banned IP 127.0.0.1 rejected",
            ]
            .iter()
            .any(|fragment| message.contains(fragment))
                || (message.contains("Policy reject")
                    && [
                        "reason=bad-message-header",
                        "reason=bad-network-magic",
                        "reason=message-payload-limit",
                        "reason=bad-message-frame",
                    ]
                    .iter()
                    .any(|reason| message.contains(reason)))
        }
        _ => false,
    }
}

impl Drop for Cluster {
    fn drop(&mut self) {
        if self.keep_alive {
            println!("[loadnet] --keep-nodes set; node processes were left running");
            return;
        }
        for node in &mut self.nodes {
            if node
                .child
                .as_mut()
                .and_then(|child| child.try_wait().ok())
                .flatten()
                .is_none()
            {
                let _ = node.rpc.admin.call("stop", json!([]));
            }
        }
        thread::sleep(Duration::from_millis(500));
        for node in &mut self.nodes {
            if let Some(child) = node.child.as_mut() {
                if child.try_wait().ok().flatten().is_none() {
                    let _ = child.kill();
                }
                let _ = child.wait();
            }
        }
    }
}
