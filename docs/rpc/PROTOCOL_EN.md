# RPC protocol (EN)

**Date:** October 17, 2026

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

## Geographic coordinate contract (version 1)

Block coordinates are **WGS 84 geographic 2D coordinates**: geodetic latitude
is north-positive and longitude is east-positive. Height and a positioning
accuracy are not implied. Reference:
[NGA WGS 84](https://earth-info.nga.mil/?action=wgs84&dir=wgs84).

- Named integer fields `latitude_ndeg` / `longitude_ndeg` use signed
  **nanodegrees**: divide by `1,000,000,000` for decimal degrees. Valid ranges
  are `[-90,000,000,000,90,000,000,000]` and `[-180,000,000,000,180,000,000,000]` respectively.
- **EPSG:4326** orders axes latitude/longitude. GeoJSON uses **OGC:CRS84**:
  **[longitude, latitude]** in decimal degrees, per
  [RFC 7946 sections 3.1.1 and 4](https://www.rfc-editor.org/rfc/rfc7946.html#section-3.1.1).
  A GeoJSON geometry has no legacy `crs` member and no invented altitude.
- `geo_tag` retains the GeoCoin v1 **0.1-degree angular cell** packing:
  `(latitude_index << 32) | longitude_index`, starting at latitude -90 and
  longitude -180. This is a GeoCoin grid, not an EPSG grid or equal-area
  tiling. Latitude indices are `0..1799`; longitude indices are `0..3599`.
  Cells are half-open at the north/east boundaries.

Verbose `getblock`, `getblockbyheight`, `getblocktemplate`, and `powdeterminism`
include `geography`. Genesis example:

```json
{
  "datum": "WGS 84",
  "crs": "OGC:CRS84",
  "coordinate_order": ["longitude", "latitude"],
  "angular_unit": "degree",
  "integer_unit": "nanodegree",
  "algorithm": "GeoPoW",
  "algorithm_version": 1,
  "grid_step_degrees": 0.1,
  "status": "exact",
  "latitude_ndeg": 48851583000,
  "longitude_ndeg": 8897278000,
  "point": {"type": "Point", "coordinates": [8.897278, 48.851583]},
  "cell": {
    "bbox": [8.8, 48.8, 8.9, 48.9],
    "center": {"type": "Point", "coordinates": [8.85, 48.85]}
  }
}
```

`bbox` follows RFC 7946 section 5: **[west,south,east,north]**. For every
non-genesis block, the exact GeoPoW point is the selected cell's centre.
Genesis retains its fixed point `[8.897278,48.851583]`, inside its original cell.
`status=exact` reports a valid point. `invalid_tag` reports out-of-grid indices;
`tag_mismatch` reports an explicitly supplied point that disagrees with its cell.
Invalid exact fields are `null`.

GeoPoW v1 selects cells globally from the **parent hash and parent's Median Time
Past**, using integer arithmetic only. Latitude-band probabilities are quantized
to 2³² tickets in proportion to WGS 84 ellipsoidal surface area; longitude uses
unbiased rejection sampling. All 1,800 latitude bands and 3,600 longitude columns
are reachable. The checked-in latitude CDF is reproduced by
`tools/generate_geopow_grid.cpp` using pinned GeographicLib 2.3.0. Cells keep their
0.1° angular size; equal probability per square metre is approximate to ticket
resolution. These are synthetic markers; no physical location measurement is
implied. Coordinate storage resolution is 10⁻⁹ degrees, not survey accuracy.

The committed `geo_tag` suffices to recover the canonical point without old MTP
headers. `listblockgeography (from_height=0, limit=100)` returns active-chain
height, hash, tag, `index_pruned`, and `geography`, including pruned history;
maximum page size is 1,000. When a header is discarded, its active hash and tag
are archived atomically in a 40-byte record. Retained headers remain the source
for recent heights. Forks never enter the historical catalogue. Reorganizations
of the retained suffix are reflected immediately.

The geographic conformance workflow compares an exact frozen fingerprint across
Linux GCC/Clang x86-64, Linux ARM64, and Windows Clang. It includes GeoPoW,
canonical metadata bytes, rounded distances/areas, and geometric decisions.
All rules and the API remain **version 1**; there is no migration parser.

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
