# GeoCoin Private Soak

Use private soak before any public mainnet launch.

Recommended layout:

- 3 internal nodes
- manual `addnode` peer lists
- no public DNS seed records yet

Goals:

- verify restart and reconnect behavior
- compare Windows and Linux outputs
- validate GUI, node, RPC, and portable bundles together

Suggested checks:

1. Start three nodes with different datadirs and ports.
2. Connect them manually with `addnode`.
3. Restart each node one by one.
4. Run GUI smoke on both Windows and Linux.
5. Run `scripts/node_rpc_smoke.py --skip-mining`.
6. Compare `powdeterminism` outputs across hosts.
