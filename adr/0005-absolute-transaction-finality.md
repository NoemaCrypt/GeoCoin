# ADR 0005: Absolute transaction finality

Status: implemented before network launch, 2026-09-13.

## Decision

`Transaction::lock_time` is an unsigned 32-bit absolute threshold. Its meaning is
shared by transaction admission, block selection, contextual block validation
and state application:

| Value | Rule |
| --- | --- |
| `0` | No absolute lock |
| `1..499999999` | Candidate block height must be strictly greater than `lock_time` |
| `500000000..4294967295` | Candidate block header Unix timestamp must be strictly greater than `lock_time` |

For transactions with inputs, `sequence = 4294967295` (`SEQUENCE_FINAL`) on
**every** input disables the absolute lock. Any other input sequence enables it.
An input-free protocol transaction has no sequence override; a nonzero threshold
must elapse. Context-free validation still checks the permitted transaction
shape and any stricter canonical requirements of a particular type.

`sequence` does not impose relative delays and does not change mempool replacement
policy. This decision does not implement relative locks.

Time locks use the **candidate header timestamp**, consistently with existing
delegation validity windows. They do not use median time past or a transaction's
own timestamp. Existing header timestamp rules remain in force, including the
allowed future-time offset. A time lock describes chain time, not an exact
wall-clock execution schedule.

## Admission and assembly

Standalone validation and mempool admission use the next chain height and local
wall-clock time. An early transaction is rejected with mempool reason `non-final`;
the mempool is not a queue for future scheduled payments.

Block selection rechecks finality using the actual candidate timestamp after
header-time adjustment. A non-final multisig root excludes its whole package.
Each new selection re-evaluates finality. Chain transitions revalidate pending
transactions, including after a pure disconnect.

Consensus validity is independent of mempool policy. Contextual block validation
and state application enforce the threshold even when the candidate did not pass
through the local mempool. Direct transaction application uses its supplied
height. Optional block-time context distinguishes an explicit zero timestamp
from the absence of block context.

## RPC and signed intent

`createrawtransaction` retains its optional `locktime` argument. If it is nonzero,
an omitted input sequence defaults to `4294967294` (`SEQUENCE_LOCKTIME`). Explicit
sequence values are preserved. With zero `locktime`, the default remains FINAL.

Verbose transaction JSON exposes `lock_time` and each input's `sequence`.
Signing preserves both fields; modifying either requires new signatures.

No serialization fields, database keys or migration path are added. The existing
transaction codec is unchanged, and RPC `api_version` remains `1`. These rules
form the initial launch contract.

## Verification

Regression coverage is in [test_transaction_finality.cpp](../tests/test_transaction_finality.cpp).
