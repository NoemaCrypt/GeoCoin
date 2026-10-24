# RPC protocol (EN)

**Date:** September 12, 2026

## Transport and format

GeoCoin implements **JSON-RPC 2.0 over HTTP**:

- HTTP method: `POST`
- Body: a request object or a batch of up to 10 requests.
- Success: `{"jsonrpc":"2.0","id":1,"result":...}`.
- Error: `{"jsonrpc":"2.0","id":1,"error":{"code":...,"message":...}}`.

Responses contain exactly one of `result` and `error`, following the
[JSON-RPC 2.0 response contract](https://www.jsonrpc.org/specification#response_object).
`getrpcinfo.api_version` is `"1"`. The initial API stays at version 1 through launch;
version 2 is reserved for a future post-launch API revision.
Clients should check whether `error` exists;
a successful response does not contain `error: null`.

A valid request without `id` is a notification. It executes without a JSON
response, including when the method fails. HTTP returns `204 No Content` for a
single notification or a batch containing only notifications. Mixed batches omit
notification responses. An explicit `"id":null` receives a response. Warmup errors
preserve request IDs and follow the same batch/notification rules.

`params` may be an array or an object keyed by `getrpcinfo.param_names`. Missing
optional named parameters use their defaults even when a later parameter is
provided. Empty `{}` is valid for a method with no parameters. Missing required
arguments and unknown parameter names are errors. As input extensions, an omitted
`jsonrpc` is accepted and `params:null` means no arguments; responses always use
`jsonrpc:"2.0"`. Malformed request objects still receive an error with an ID of
`null` when no valid ID is available.

Integer parameters reject booleans, fractional JSON numbers, negative values for
unsigned fields, and values outside the destination type's range. They are never
silently truncated or wrapped. GC amounts use the separate exact amount parser.

Implementation: `src/geocoin/rpc/rpc_protocol.cpp` (HTTP),
`src/geocoin/rpc/rpc_server.cpp` (lifecycle/dispatch), and
`src/geocoin/rpc/rpc_server_security.cpp` (authentication/rate limits).

## Authentication (required)

HTTP Basic Auth (user/pass) is mandatory.

Important: if `rpc_user` or `rpc_password` is empty, server startup is rejected
(fail-closed). See `RPCServer::start` and `RPCServer::authenticate`.

Authentication failures receive exponential backoff. Request rate limiting is
off by default (`rpc-rate-limit=0`); a non-zero budget returns
`429 Too Many Requests` when it is exceeded.

## Request example

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "getblockhash",
  "params": [100]
}
```

## Examples (curl)

Recommended environment variables:

```bash
export RPC_URL="http://127.0.0.1:8778/"
export RPC_USER="user"
export RPC_PASS="pass"
```

Default ports depend on the selected network (or override with `--rpc-port`); the node prints them on startup:
- mainnet (`degree`): `3033`
- testnet (`degree-test`): `13033`

Call `help`:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"help","params":[]}' \
  "$RPC_URL"
```

Get help for one method:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"help","params":["getblock"]}' \
  "$RPC_URL"
```

## Errors

RPC errors are returned in `error`:

```json
{
  "jsonrpc": "2.0",
  "error": { "code": -32602, "message": "..." },
  "id": 1
}
```

Error codes: `include/geocoin/rpc/rpc_types.hpp` (`RPCErrorCode`).

## Help / introspection

- `help` lists methods (grouped by `category`) or shows a method help string.
  - `help`
  - `help "getblock"`
- `getrpcinfo` returns the machine-readable registry: `api_version`, `name`, `category`, `role`, `capabilities`, `requires_wallet`, `param_names`, `mutates`, `side_effect_free`, `stability`, `since`, `deprecated`, `deprecation`, `side_effects`.

For exact parameter contracts, prefer the built-in `help` output.

## RPC Roles

RPC credentials are split by role:

- `admin` — wallet/admin operations plus readonly methods.
- `readonly` — methods whose registry role is `readonly`.
- `miner` — external mining only (`getblocktemplate`, `submitblock` and miner telemetry).

Roles are not a full hierarchy: `admin` does not call miner-only methods, and `miner` does not call regular readonly methods. Check the `role` field with `getrpcinfo`.
