# GeoCoin RPC Reference (EN)

**Date:** April 16, 2026
**Transport:** HTTP + JSON-RPC 2.0 (Basic Auth)

This section documents the node RPC surface for `geocoin-node`, grouped by subsystem. `degree_node` is a compatibility alias.

Each page below includes an **Examples (curl)** section with ready-to-run JSON-RPC calls.

## Quick start

Minimal JSON-RPC request example (`getblockcount`):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" \
  -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  "http://127.0.0.1:$RPC_PORT/"
```

The port depends on the selected network (see `geocoin-node` startup output or `consensus::Params().rpc_port()`):
- mainnet: `3033`
- testnet: `13033`

## Pages

- Protocol, auth, errors: `PROTOCOL_EN.md`
- Stress tests: `STRESS_EN.md`
- Blockchain API (chain/blocks/tx query): `BLOCKCHAIN_EN.md`
- Wallet API (local wallet, send, legacy account methods): `WALLET_EN.md`
- Network API (peers/connections/P2P policy stats): `NETWORK_EN.md`
- Mining API (`miner` credentials, block templates, node-local mining): `MINING_EN.md`
- Misc/Control/Util (help/stop/signatures): `MISC_EN.md`

Generate a compact runtime registry from `getrpcinfo`:

```bash
python3 scripts/generate_rpc_docs.py --url "$RPC_URL" --user "$RPC_USER" --password "$RPC_PASS"
```

## See also

- Architecture (RU): `../ARCHITECTURE_RU.md`
- Docs index: `../README.md`
