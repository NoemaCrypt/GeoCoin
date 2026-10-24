# Network RPC (EN)

**Date:** April 16, 2026
**Source:** `src/geocoin/rpc/rpc_network.cpp`

Methods for monitoring P2P state and managing peers.

## Methods

- `getpeerinfo` — connected peers and their state.
- `getconnectioncount` — current connection count.
- `getaddednodeinfo` — persistent `addnode` targets and connection state.
- `getp2ppolicystats` — P2P policy rejection and temporary-ban counters.
- `listbans` — active manual bans.
- `addnode <ip:port> <add|remove|onetry>` — add/remove peer or try once.
- `disconnectnode <ip[:port]>` — disconnect a peer.
- `setban <ip|subnet> <add|remove> [bantime]` — add/remove a manual ban.
- `clearbans` — clear manual bans.

Only aggregate `getconnectioncount` and `getp2ppolicystats` are available to
`readonly`. Peer-address and ban details exposed by `getpeerinfo`,
`getaddednodeinfo`, and `listbans` require `admin`; so does
`getconnectiondiagnostics`. Peer and ban-list management methods also require
`admin` and are marked as `mutates=true` in `getrpcinfo`.

Use `help "<method>"` for exact parameter/result formats.

## Examples (curl)

List peers (admin credential):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getpeerinfo","params":[]}' \
  "$RPC_URL"
```

Add a peer (example):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"addnode","params":["1.2.3.4:3030","add"]}' \
  "$RPC_URL"
```
