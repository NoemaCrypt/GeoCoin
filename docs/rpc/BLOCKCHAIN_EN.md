# Blockchain RPC (EN)

**Date:** September 12, 2026
**Source:** `src/geocoin/rpc/rpc_blockchain_*.cpp`, `src/geocoin/rpc/rpc_mempool.cpp`

These methods read data from the active chain (`Blockchain`/`BlockStorage`) and state (`ChainState`), and expose pruning controls.

## Methods

- `getblockcount` — active chain height.
- `getblockrejectstats` — aggregated block rejection stats (consensus/policy).
- `getwitnesspubkeystats` — witness pubkey cache/savings stats.
- `getinterestpool` — interest pool state (network-dependent).
- `getblockhash <height>` — active-chain block hash at height.
- `getprunedrange` — available/pruned data range.
- `getblockavailability <hash|height>` — block and witness payload availability.
- `getavailableblocks [start] [count]` — available-block window.
- `getblock <hash> [verbose]` — fetch block by hash.
- `getblockbyheight <height> [verbose]` — fetch active-chain block by height.
- `getblockchildren <hash>` — child blocks (fork index).
- `getblocktree <hash> [max_depth] [max_nodes]` — bounded block tree from a
  given node (defaults `2`/`1000`, maximum `32`/`5000`).
- `switchtotip <hash>` — switch active tip (admin/diagnostics).
- `getblockchaininfo` — chain/network summary, including the persistent `consensus_ruleset` identifier.
- `getrawtransaction <txid> [verbose]` — fetch a transaction (mempool/chain).
- `getdelegationproof <txid>` — get delegation proof by txid.
- `gettxout <txid> <vout>` — current unspent output, or `null` when spent/unknown.
- `getaddressbalance <address>` — address balance from state.
- `getaddresstxids <address> [offset] [limit]` — bounded active-chain txid page
  from the address tx index.
- `getaddresshistory <address> [verbose] [offset] [limit]` — bounded address
  history page from the address tx index.
- `getnftshistory <nft_id> [verbose] [offset] [limit]` — bounded NFT id history
  page from the NFT tx index.
- `getaddressutxos <address>` — address UTXOs from state.
- `getfundbalance` — foundation/fund address balance.
- `getsidechaingenesis <sidechain_id>` — sidechain genesis, profile (`financial`/`accounting`), and lifecycle status.
- `createsidechaingenesis <payload|fields>` — deterministically compute a sidechain genesis hash; for accounting chains, version-2 payloads bind `sidechain_kind` and `schema_hash`; it does not mutate the chain.

### Active chain and transaction lookup

Verbose block results include `in_active_chain`. Confirmations are calculated from
one chain-index snapshot: an active block has `tip_height - height + 1`, an indexed
alternative block has `-1`, and a block missing from the index has `0`. Only active
blocks expose `nextblockhash`. `getblockhash` reads the active index without loading
a block body; configured pruning access limits still apply.

`getblockavailability.found` and `in_active_chain` describe membership in the active
chain. A stored alternative block returns `false` for these fields, even if its
body exists. `getrawtransaction` reads retained chain transactions and then the
local mempool, so newly accepted transactions are available before confirmation.

### Pruning / witness

- `prunenow [target_height] [max_blocks]` — force witness pruning up to height.
- `unprune <block_hash|height>` — request witness data from peers to restore a pruned block.

Read methods in this section are available to `readonly`. `switchtotip`, `prunenow`, and `unprune` require `admin` and are marked as `mutates=true`.

### Mempool / raw tx preflight

- `getmempoolinfo` — read-only mempool stats; available to `readonly` and `miner`.
- `getrawmempool [verbose] [offset] [limit]` — deterministic txid-ordered
  mempool page. Non-verbose pages default to `1000` and are capped at `5000`;
  verbose pages default to `100` and are capped at `250`.
- `getmempoolentry <txid>` — one mempool transaction entry.
- `testmempoolaccept <hex|[hex...]>` — checks raw transaction(s) without submitting them.

Address/NFT history pages use the same `1000`/`5000` non-verbose and
`100`/`250` verbose defaults/caps. Increase `offset` until an empty page is
returned. `getmempoolinfo.size` reports the current mempool total.

Details: `../../src/geocoin/storage/block_storage.cpp`.

## Example

Fetch a block by height:

1) `getblockhash`
2) `getblock`

Use `help "getblockhash"` and `help "getblock"` for exact parameter/result formats.

### Example (curl)

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":[100]}' \
  "$RPC_URL"

# Put the hash from result here:
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"getblock","params":["<block-hash>",true]}' \
  "$RPC_URL"
```
