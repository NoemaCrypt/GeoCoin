# Interest pool — holder dividends after the first halving

> Русская версия: [docs/INTEREST_POOL_RU.md](INTEREST_POOL_RU.md)

This document describes the mechanism that redistributes the foundation
share to coin holders (the interest pool) exactly as it is implemented in
consensus today, including the current limitation and the plan to remove
it. It is written for exchanges, auditors, and the community: every
parameter here is verifiable against the code and the chain state.

## 1. Two phases of the block-reward split

### Phase 1 — blocks 1 … 209,999 (current)

10% of the block reward (block reward + transaction fees) is paid as a
coinbase output to the foundation address:

- address: `GEOM3PyazNssCYYzF3tW9YrsSwszLq45QXbAZv`;
- policy: **3-of-4 multisig**, participant addresses are fixed in
  `include/geocoin/consensus/consensus.hpp` (`FOUNDATION_*`);
- rule: `FOUNDATION_REWARD_PERCENT = 10` (the "90/10 rule");
- the genesis reward is directed to the same address.

### Phase 2 — from block 210,000 (the first halving)

At `INTEREST_POOL_ACTIVATION_HEIGHT = 210,000` (the same height as the
first halving: reward 50 → 25 GEO):

- the foundation share **is no longer paid as a coinbase output** to the
  multisig;
- it is credited to an internal **interest pool** — chain-state metadata
  (`interest_pool_balance_`), validated by consensus on every applied
  block (`src/geocoin/state/state_block.cpp`, the
  `is_foundation_reward_redirected_to_pool` branch);
- the flow into the pool after the halving: 10% × 25 GEO = 2.5 GEO per
  block.

The point: the network stops "collecting a tax into a foundation" and
starts **paying dividends to its own holders** — emission drops with the
halving while the freed share is returned to the holder economy.

## 2. Quarterly distribution

- Interval: `INTEREST_DISTRIBUTION_INTERVAL = 13,104` blocks
  (91 days × 144 blocks/day ≈ one quarter at 10-minute blocks).
- At each quarterly height, consensus **requires** exactly one
  `INTEREST_DISTRIBUTION` transaction (positioned immediately after the
  coinbase). Outside a quarterly height such a transaction is forbidden;
  if the mandatory distribution is missing, the block is invalid
  (`state_block.cpp`, the INTEREST_DISTRIBUTION section).
- The distribution is deterministic: every node recomputes the expected
  outputs and compares them with the transaction — a miner cannot
  distribute the pool "as it likes".

## 3. Payout rules (`build_interest_distribution_outputs_`)

Implementation: `src/geocoin/state/state_foundation.cpp:236`.

1. Balances are taken **per address at the preceding chain tip**. UTXOs
   created at the distribution height are excluded from the calculation —
   this is a farming barrier: dust cannot be mined or sent in the same
   block to earn a share.
2. Each address payout is proportional to its share of the total balance:
   `payout = pool × balance / total_balance` (exact integer division via a
   128-bit mul-div, without consensus loss).
3. Payouts below the dust limit (`DUST_LIMIT = 0.00001 GC`) are skipped —
   the second barrier against dust farms built from many tiny UTXOs.
4. The result is published completely or not at all: partial payouts are
   impossible, and the transaction passes the full consensus format check
   before being included.

## 4. Known limitation (stated openly) and the plan

A single distribution transaction holds at most `MAX_TX_OUTPUTS = 1000`
outputs. If a correct distribution cannot be built (the number of
payee addresses exceeds 1000), the builder returns an error, and under
the current consensus logic the distribution for that quarter **does not
happen** — the pool carries over to the next quarter.

- Activation happens at block 210,000 — roughly 4 years after mainnet
  launch at 10-minute blocks. There is ample time to remove the
  limitation, and it **does not affect the running phase 1**.
- Plan before activation: split the distribution across several
  transactions in the quarterly block (mirroring how the current single
  transaction is positioned), or select the top-N addresses with the
  remainder of the pool carried to the next period. The chosen variant
  will be fixed in this document and in an ADR before
  `INTEREST_POOL_ACTIVATION_HEIGHT`.

## 5. Reference numbers

| Parameter | Value |
| --- | --- |
| Initial block reward | 50 GEO |
| Foundation share | 10% (reward + fees) |
| Foundation multisig | 3-of-4, until block 210,000 |
| First halving = interest pool activation | block 210,000 |
| Reward after the first halving | 25 GEO (2.5/block into the pool) |
| Distribution interval | every 13,104 blocks (≈91 days) |
| Payout dust limit | 0.00001 GC |
| Outputs per distribution transaction | 1000 (see section 4) |

## 6. Code references

- `include/geocoin/consensus/consensus.hpp` — phase parameters,
  activation height, interval, foundation-cut calculation;
- `src/geocoin/state/state_block.cpp` — pool accrual and the mandatory
  quarterly transaction;
- `src/geocoin/state/state_foundation.cpp` — deterministic payout
  calculation;
- `src/geocoin/consensus/proof_of_work.hpp` — block reward and halvings.
