# Misc / Control / Util RPC (EN)

**Date:** April 16, 2026
**Source:** `src/geocoin/rpc/rpc_misc.cpp`

Service methods (node control, utilities, signatures).

## Methods

- `help ["method"]` — list methods or show help for a specific method.
- `getrpcinfo` — machine-readable method registry with role, category, mutability, and deprecation metadata.
- `stop` — request server shutdown (control contract).
- `getinfo` — compact summary (version, height, balance, crypto).
- `getnodeinfo` — node, network, RPC, and storage backend summary.
- `getsyncstatus` — sync status.
- `gethealth` — compact health check.
- `getmetrics` — internal metrics snapshot.
- `getstorageinfo` — data-dir/storage layout and sizes (admin diagnostics).
- `getdbstats` — DB/storage statistics (admin diagnostics).
- `getpruninginfo` — pruning mode and state.
- `validateaddress <address>` — validate an address without exposing wallet ownership (read-only).
- `getwalletaddressinfo <address>` — validate an address and report `ismine` (admin/wallet capability).

### Signatures (Falcon-512)

- `signmessage <address> <message>` — sign a message with the address key (wallet required).
- `verifysignature <address> <signature_hex> <message>` — verify a signature.
- `getfalconinfo` — Falcon-512 diagnostics (see `help "getfalconinfo"`).

### PoW diagnostics

- `powdeterminism` — GeoPoW determinism self-check.

Use `help "<method>"` for exact parameter/result formats.

## Examples (curl)

`validateaddress`:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"validateaddress","params":["GEO..."]}' \
  "$RPC_URL"
```

Sign and verify:

```bash
# signmessage (wallet must be loaded)
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"signmessage","params":["GEO...","hello"]}' \
  "$RPC_URL"

# verifysignature
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":3,"method":"verifysignature","params":["GEO...","<sig-hex>","hello"]}' \
  "$RPC_URL"
```
