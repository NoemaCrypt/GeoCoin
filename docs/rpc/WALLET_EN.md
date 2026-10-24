# Wallet RPC (EN)

**Date:** April 16, 2026
**Source:** `src/geocoin/rpc/rpc_wallet.cpp`

These methods manage the local wallet and build/broadcast transactions. Public chain/mempool/NFT getters are categorized as `blockchain`, `mempool`, and `nft` in `getrpcinfo`, even when they are documented next to wallet workflows.

## 1) Wallet and addresses

- `createwallet` — create/initialize the wallet.
- `getnewaddress` — generate a new address.
- `importprivkey <hex> [label] [password] [rescan=true] [start_height=0]` — import a private key and restore its active-chain history by default.
- `dumpprivkey <address>` — export a private key.
- `getaddressesbyaccount <account>` — legacy account-style address list; marked deprecated in `getrpcinfo` for new clients.

`importprivkey` returns `rescan_requested`, `rescan_complete`, `rescan_status`,
and `next_height`. A `read-error`, `history-pruned`, or `chain-changed` status
means that the key was saved but its history is incomplete. Restore access to
the block bodies and repeat `importprivkey` with the same key and `start_height`
set to `next_height`. Re-importing the same key is idempotent. Pass `rescan=false`
to disable history recovery explicitly.

## 2) Balances and UTXO

- `getbalance` — total wallet balance.
- `listaddressbalances` — per-address balances.
- `listunspent` — wallet UTXOs.
- `getaddressutxos <address>` — UTXOs for an address (`readonly`, `blockchain` category).
- `getutxos <address>` — legacy single-address alias for `getaddressutxos`, deprecated.
- `getfeerate` — fee rate estimate (`readonly`, `mempool` category).
- `getfundbalance` — fund/special-account balance (`readonly`, `blockchain` category).

## 3) Sending

- `sendtoaddress` — build/sign/broadcast a transaction.
- `sendrawtransaction <hex>` — broadcast a raw transaction (verify + mempool).
- `verifyrawtransaction <hex>` — verify a raw tx without mempool admission (`readonly`, `mempool` category).
- `listtransactions` — legacy account-style wallet transaction history; deprecated for new clients.

### Raw transaction builder

- `createrawtransaction <inputs> <outputs> [locktime]` — builds an unsigned STANDARD raw transaction. Output `amount`/`value` fields are GC; `value_angles` is the exact integer amount in angles.
- `decoderawtransaction <hex>` — decodes raw transaction JSON.
- `signrawtransactionwithwallet <hex>` — signs a raw transaction with local wallet keys when all inputs belong to one local address.

Recommended external-client pipeline: `createrawtransaction` → `signrawtransactionwithwallet` → `testmempoolaccept`/`verifyrawtransaction` → `sendrawtransaction`.

Verbose decoding also returns `inputs[].script_sig_hex`,
`outputs[].script_pubkey_hex`, `witness_version`, and `witnesses[]` containing
`public_key_hash`, `public_key_hex`, and `signature_hex`. Ordinary inputs expose
their body `signature_hash`; ANGLE_COMMITMENT inputs expose the body
`signature_hex`. An empty hex string means those bytes are absent from the
transaction. Decoding does not substitute keys from storage. Empty witness
slots in unsigned transactions are displayed as encoded in raw bytes.

`memo` is a local wallet history note. It is not part of raw transactions, txids,
or `decoderawtransaction` output. Fields belonging to another transaction type
cannot serve as extra metadata: a populated inactive payload is rejected before
signing or persistence.

GeoCoin uses address-based UTXOs. `script_sig` and `script_pubkey` are opaque
body bytes and are not executed. Coinbase commits height in the first 8 bytes
of `script_sig`; ordinary input signatures reside in witness. Each script
field is limited to 10000 bytes. Sidechain payload builders enforce this
limit even with `validate=false`.

`locktime = 0` disables the lock. Values below `500000000` specify a height;
other values specify the block header Unix timestamp. The threshold must be
strictly passed: `locktime = 100` permits inclusion from block 101.
With nonzero `locktime`, an omitted `input.sequence` becomes `4294967294`;
explicit values are preserved. Setting **every** input sequence to `4294967295`
disables the lock. Other sequence values enable only the absolute lock, with no
relative delay. Decoding exposes `lock_time` and each input's `sequence`;
wallet signing preserves both fields.

Mempool admission uses the next height and current local time and rejects early
transactions with reason `non-final`. A signed raw transaction can be prepared
in advance and submitted later. Block selection checks the actual candidate
time again. This is chain time subject to the header's allowed offset, not an
exact wall-clock schedule. See [ADR 0005](../../adr/0005-absolute-transaction-finality.md).

## 4) NFT

- `getnft <nft_id>` — fetch an NFT by id (`readonly`, `nft` category).
- `listnftsbyowner <address>` — list NFTs by owner (`readonly`, `nft` category).

Payload→finalize workflow:
- `createnftcreatepayload` → `finalizenftcreate`
- `createnfttransferpayload` → `finalizenfttransfer`
- `createnftburnpayload` → `finalizenftburn`

NFT metadata has one shared parser and formatter for signing payloads, local
`createnft`, `getnft`, and decoded transactions. `content_hash` is a required
32-byte hex string. `polygon` contains 3..`MAX_NFT_POINTS` points in integer
microdegrees, as `{lat_udeg,lon_udeg}` objects or `[lat_udeg,lon_udeg]` pairs;
latitude is within ±90,000,000 and longitude within ±180,000,000. The old
`lat1`/`lon1` triangle fields are not accepted.

Optional metadata fields: `name`, `description`, `image_uri`, `area_bits`,
`created_at`, `block_height`, `block_hash`, `location_name`, `creator`, `tags`,
`cadastral`. Present hashes and collections are validated; malformed values
are not silently discarded. Cadastral monetary fields are exclusively
`market_value_angles` and `cadastral_value_angles`, non-negative integer angles.
Other cadastral fields are `cadastral_number`, `owner_name`, `land_category`,
`permitted_use`, `area_sqm_official`, `ownership_document_hash`, `technical_plan_hash`,
`photo_hashes`, `last_valuation_date`, `restrictions`, `has_buildings`, `building_count`.
Unknown metadata/cadastral fields are rejected. `getnft.metadata` includes
`content_hash` and can be parsed back without losing serialized metadata fields.

Supply all signing-relevant metadata explicitly when reproducing an external
signature: omitted creation time, block context and creator default to zero/empty
in `createnftcreatepayload`, whereas local `createnft` supplies the current context.
`royalty_percent` is an integer in basis points, from `0` through `10000`.

## 5) Sidechain

Sidechain genesis RPC is in the blockchain section:
- `getsidechaingenesis`
- `createsidechaingenesis`

Most sidechain wallet operations follow:
1) `create...payload` (often `validate=true` by default)
2) signatures
3) `finalize...` returns the final `tx_hex`/`txid`. These methods do not broadcast to the mempool; use `sendrawtransaction` or a specialized `send...package` method to submit.

Methods:

- `createsidechainissuepayload` / `finalizesidechainissue`
- `createaccountingsidechainpayload` / `finalizeaccountingsidechain`
- `createsidechainmintpayload` / `finalizesidechainmint`
- `createsidechainredeempayload` / `finalizesidechainredeem`
- `createsidechainslashpayload` / `finalizesidechainslash`
- `createsidechainlockpayload` / `finalizesidechainlock`
- `createsidechaintopuppayload` / `finalizesidechaintopup`
- `createsidechainspendpayload` / `finalizesidechainspend`
- `createsidechainunlockpayload` / `finalizesidechainunlock`

Checkpoints:

- `createsidechaincheckpointpayload` / `finalizesidechaincheckpoint`
- `createcheckpointproofv2`
- `createaccountingcheckpointproofv1`
- `createbatchsidechaincheckpointpayload` / `finalizebatchsidechaincheckpoint`
- `createsidechaincommitteeupdatepayload` / `finalizesidechaincommitteeupdate`
- `createsidechaindecommissionpayload` / `finalizesidechaindecommission`

Financial sidechains use checkpoint policy/version `2/2` and support
mint/redeem. Accounting sidechains use `3/1`, commit a namespace and schema
hash, and have no token economics. Decommissioned registries remain slashable
for a 144-block challenge period before `UNLOCK_DEPOSIT` removes the registry
and deposit.

## 6) Multisig / Delegation

- `sendmultisigpackage`
- `registermultisig`
- `createdelegationproof`
- `createrevokedelegation`
- `senddelegationpackage`
- `sendrevokepackage`

## Notes

- For exact parameter contracts, use `help "<method>"` (the help strings in code are the source of truth).

## Examples (curl)

Get balance:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getbalance","params":[]}' \
  "$RPC_URL"
```

Broadcast a raw transaction:

```bash
RAW_TX_HEX="<hex>"

# Quick verify without mempool admission:
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"verifyrawtransaction\",\"params\":[\"$RAW_TX_HEX\"]}" \
  "$RPC_URL"

# Mempool submission (if valid):
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"sendrawtransaction\",\"params\":[\"$RAW_TX_HEX\"]}" \
  "$RPC_URL"
```
