# Mining RPC (EN)

**Date:** April 16, 2026
**Source:** `src/geocoin/rpc/rpc_mining.cpp`

## Methods

- `getmininginfo` — mining summary (status/params/height/difficulty).
- `getmempoolinfo` — read-only mempool stats for transaction selection.
- `getnetworkhashps` — estimated network hashrate.
- `getblocktemplate {"address":"..."}` — block template for external miners.
- `submitblock <hex>` — submit a found block.
- `setgenerate <true|false> [threads]` — enable/disable mining in the node.

`getmininginfo`, `getmempoolinfo`, `getnetworkhashps`, `getblocktemplate`, and `submitblock` require separate `miner` RPC credentials. `setgenerate` controls node-local CPU mining, requires `admin`, and requires a wallet.

Use `help "<method>"` for exact parameter/result formats.

`getblocktemplate.geography` describes the same WGS 84 point committed by
`geo_tag` in `blockhex`, using the [version 1 geographic contract](PROTOCOL_EN.md#geographic-coordinate-contract-version-1).
The full point is reconstructed from the template's parent, not its `curtime`.

## Examples (curl)

Mining info:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' \
  "$RPC_URL"
```

Enable mining (example, 4 threads):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"setgenerate","params":[true,4]}' \
  "$RPC_URL"
```

Template for an external miner:

```bash
curl -sS -u "$RPC_MINER_USER:$RPC_MINER_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":3,"method":"getblocktemplate","params":[{"address":"GEO..."}]}' \
  "$RPC_URL"
```
