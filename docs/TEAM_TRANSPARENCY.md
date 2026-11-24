# Team transparency

> Русская версия: [TEAM_TRANSPARENCY_RU.md](TEAM_TRANSPARENCY_RU.md)

This document publicly records **who controls GeoCoin funds and what happens
to them**: foundation governance, spending categories, the interest pool
mechanism, and how to verify all of it independently. It is written for
everyone — holders, miners, exchanges, and corporate clients — not only for
a single exchange's reviewers.

## 1. Who controls the foundation funds

The foundation funds belong not to "the team" but to a **3-of-4 multisig
address**. No participant — including the project founders — can move funds
alone: spending requires signatures from at least three of the four keys.

| Parameter | Value |
| --- | --- |
| Foundation address | `GEOM3PyazNssCYYzF3tW9YrsSwszLq45QXbAZv` |
| Policy | 3 signatures of 4 (`FOUNDATION_M = 3`, `FOUNDATION_N = 4`) |
| Participants | Hardcoded in `consensus.hpp` (`FOUNDATION_PARTICIPANTS`) |
| Policy change | Only via the `UPDATE_FOUNDATION_ADDRESS` consensus transaction with a 3-of-4 quorum |

All parameters are hardcoded in
`include/geocoin/consensus/consensus.hpp` and can only change through a
consensus upgrade, not by an off-chain decision.

## 2. What the foundation receives

- **The genesis subsidy** — 50 GC to the foundation address from the
  mainnet genesis block;
- **10% of every block's reward** (`FOUNDATION_REWARD_PERCENT = 10`)
  including fee shares, at heights 1–209,999.

Order of magnitude: at a 50 GC block reward the foundation receives ~5 GC
per block (plus 10% of fees). The accumulated amount is visible to anyone
on the [explorer](https://explorer.geocoin.network/) under the foundation
address.

## 3. What the foundation funds are used for

**1. Project support (continuous).** Infrastructure and development: seed
nodes and network reliability, the block explorer, exchange-integration
tooling, documentation, and security audits.

**2. Genesis handover and geo-puzzles.** The network's launch point is
**48.851583° N, 8.897278° E — the Porsche Development Center, Weissach,
Germany** (Porschestraße): the mainnet genesis geotag points exactly there;
that is where we started. A share of the budget funds the symbolic handover
of the genesis block to the aerodynamics ("wing") development team at that
location, and a series of geolocation puzzle events — an engagement
mechanic built on the network's own geo-anchoring (GeoPoW, cadastral
geotags). This is the main community-growth engine.

**3. First corporate sidechain clients.** Registering a sidechain on L1
requires locked deposit collateral (`LOCK_DEPOSIT`). The foundation
subsidizes the collateral deposits of the first corporate clients, lowering
their entry cost and creating live B2B usage of the chain.

**4. Exchange liquidity.** A dedicated share supports liquidity for GEO
markets, including the locked liquidity required for listed pairs.

## 4. Reporting

- **Every foundation expense is verifiable on-chain**: anyone can open the
  foundation address in the [explorer](https://explorer.geocoin.network/)
  and see incoming block subsidies and outgoing spends.
- Significant expenses (listings, corporate deposit subsidies, audits) are
  additionally published by the team in the [organization's
  Discussions](https://github.com/NoemaCrypt/.github/discussions) — with
  txids and a short description.
- This document contains no promises of "the foundation will spend N% on X"
  without on-chain confirmation: the document defines the spending
  categories; the facts are checked on the explorer.

## 5. Interest pool — after the first halving the foundation share goes to holders

From block **210,000** (the first halving) the 10% foundation share is no
longer paid to the multisig; it accrues consensus-wise to the internal
**interest pool**, and every **13,104 blocks (≈ a quarter)** it is
distributed deterministically, pro rata, to the balances of all holders:

- UTXOs created at the distribution height are excluded — you cannot "mine"
  a share in the last block;
- payouts below the dust limit (0.00001 GC) are skipped — dust farms do not
  work;
- the distribution is mandatory under consensus and computed identically by
  every node.

The full mechanics, parameters, and the known limitation with its fix plan:
[INTEREST_POOL.md](INTEREST_POOL.md) (RU:
[INTEREST_POOL_RU.md](INTEREST_POOL_RU.md)).

## 6. References

- Code: `include/geocoin/consensus/consensus.hpp` (foundation and pool
  parameters), `src/geocoin/state/state_foundation.cpp` (distribution),
  `src/geocoin/state/state_block.cpp` (pool accrual);
- Exchange integration: [exchange-integration.txt](exchange-integration.txt);
- Defects found during testing and open risks:
  [PRODUCTION_TESTING.md](PRODUCTION_TESTING.md).
