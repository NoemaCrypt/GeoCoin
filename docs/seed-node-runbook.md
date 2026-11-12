# GeoCoin Seed Node Runbook

Target host:

- Ubuntu 22.04 x86_64
- archive or full-history mode
- public `TCP 3030`
- RPC on loopback only

Bring-up steps:

1. Create user `geocoin`.
2. Create datadir `/var/lib/geocoin`.
3. Install dependencies with `scripts/bootstrap-ubuntu-22.04.sh` or deploy the portable Linux bundle.
4. Copy `activation-manifest.json` to `/var/lib/geocoin/degree/activation-manifest.json`.
5. Create `/var/lib/geocoin/degree` and copy `deploy/geocoin.conf.seed.example`
   to `/var/lib/geocoin/degree/geocoin.conf`. Configuration is network-scoped.
6. Create `/etc/geocoin/rpc.pass`, owned by `geocoin:geocoin` with mode `0600` and a strong random password on its first line.
7. Install `deploy/systemd/geocoin-node.service`.
8. Run `deploy/firewall/geocoin-node-ufw.sh`.
9. In the provider security group, allow only `22/tcp` and `3030/tcp`; explicitly deny `3033/tcp` and every unrelated public port.
10. From an independent external host, verify `3030/tcp` connects and `3033/tcp` does not connect.
11. Wait for sync and validate `getblockcount` and `getpeerinfo` through loopback RPC using the configured password file. Every ready peer must report `transport_encrypted=true`, `secure_wire_version=2`, and `secure_suite=1`.
12. Publish only verified bootstrap endpoints. The configured mainnet endpoint is
    `45.12.114.4:3030`; no DNS seed hosts are currently configured.

All seeds and miners must use the permanent consensus rules from genesis.
Do not reuse a datadir produced by the discarded launch-rule test chain; reset
the seed and synchronize it from independently initialized peers. The daemon
rejects non-empty block databases without the current consensus ruleset marker.

If external TCP connect succeeds on random closed ports, the provider-side
security group is still open even if Linux `ufw` is correct. Fix this in the
provider panel before public launch.
