# Blockchain RPC (RU)

**Дата:** 12 сентября 2026
**Источник:** `src/geocoin/rpc/rpc_blockchain_*.cpp`, `src/geocoin/rpc/rpc_mempool.cpp`

Методы этого раздела читают данные из активной цепи (`Blockchain`/`BlockStorage`) и состояния (`ChainState`) и управляют pruning.

## Методы

- `getblockcount` — текущая высота активной цепи.
- `getblockrejectstats` — агрегированная статистика отказов блоков (consensus/policy).
- `getwitnesspubkeystats` — статистика по witness pubkey cache/экономии.
- `getinterestpool` — состояние interest pool (если включено правилами сети).
- `getblockhash <height>` — хеш блока активной цепи на высоте.
- `getprunedrange` — диапазон доступных/pruned данных.
- `getblockavailability <hash|height>` — доступность блока и witness payload.
- `getavailableblocks [start] [count]` — окно доступных блоков.
- `getblock <hash> [verbose]` — получить блок по хешу.
- `getblockbyheight <height> [verbose]` — получить блок активной цепи по высоте.
- `getblockchildren <hash>` — получить дочерние блоки (fork index).
- `getblocktree <hash> [max_depth] [max_nodes]` — ограниченное дерево от
  указанного блока (по умолчанию `2`/`1000`, максимум `32`/`5000`).
- `switchtotip <hash>` — переключить активный tip на указанный (для диагностики/администрирования).
- `getblockchaininfo` — сводная информация о цепочке/сети, включая постоянный идентификатор `consensus_ruleset`.
- `getrawtransaction <txid> [verbose]` — получить транзакцию (mempool/chain).
- `getdelegationproof <txid>` — получить транзакцию/объект delegation proof по txid.
- `gettxout <txid> <vout>` — получить текущий unspent output или `null`, если выход потрачен/не найден.
- `getaddressbalance <address>` — баланс адреса из состояния.
- `getaddresstxids <address> [offset] [limit]` — ограниченная страница txid
  активной цепи по address-index.
- `getaddresshistory <address> [verbose] [offset] [limit]` — ограниченная
  страница истории адреса из address-index.
- `getnftshistory <nft_id> [verbose] [offset] [limit]` — ограниченная страница
  истории NFT id из NFT tx-index.
- `getaddressutxos <address>` — UTXO адреса из состояния.
- `getfundbalance` — баланс foundation/fund адреса.
- `getsidechaingenesis <sidechain_id>` — genesis, профиль (`financial`/`accounting`) и статус sidechain.
- `createsidechaingenesis <payload|fields>` — детерминированно посчитать sidechain genesis hash; для accounting поля `sidechain_kind` и `schema_hash` включаются в payload версии 2; цепь не мутирует.

### Активная цепь и поиск транзакций

Подробный ответ о блоке содержит `in_active_chain`. Подтверждения вычисляются по
одному снимку индекса цепи: для активного блока — `tip_height - height + 1`, для
индексированного блока другой ветки — `-1`, для блока без индекса — `0`.
`nextblockhash` возвращается только для активной цепи. `getblockhash` читает
активный индекс без загрузки тела блока; ограничения доступа при pruning остаются.

В `getblockavailability` поля `found` и `in_active_chain` означают принадлежность
активной цепи. Для сохранённого блока другой ветки они равны `false`, даже если
тело блока есть. `getrawtransaction` ищет сохранённую транзакцию цепи, затем
обращается к локальному mempool: принятая транзакция доступна до подтверждения.

### Pruning / witness

- `prunenow [target_height] [max_blocks]` — принудительный witness pruning до высоты.
- `unprune <block_hash|height>` — запросить witness у пиров для восстановления pruned блока.

Все методы чтения в этом разделе доступны для `readonly`. `switchtotip`, `prunenow`, `unprune` требуют `admin` и помечены как `mutates=true`.

### Mempool / raw tx preflight

- `getmempoolinfo` — read-only статистика mempool; доступен `readonly` и `miner`.
- `getrawmempool [verbose] [offset] [limit]` — детерминированная страница
  mempool в порядке txid. Обычная страница: `1000` по умолчанию, максимум
  `5000`; verbose: `100` по умолчанию, максимум `250`.
- `getmempoolentry <txid>` — metadata одной mempool-транзакции.
- `testmempoolaccept <hex|[hex...]>` — проверить raw transaction без отправки.

Истории адресов/NFT используют те же значения `1000`/`5000` для обычного и
`100`/`250` для verbose-режима. Увеличивайте `offset`, пока ответ не станет
пустым. Текущий общий размер mempool возвращает `getmempoolinfo.size`.

Подробно: `../../src/geocoin/storage/block_storage.cpp`.

## Пример

Получить блок по высоте:

1) `getblockhash`
2) `getblock`

Точные параметры и формат результата см. в `help "getblockhash"` и `help "getblock"`.

### Пример (curl)

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":[100]}' \
  "$RPC_URL"

# Подставьте hash из result:
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"getblock","params":["<block-hash>",true]}' \
  "$RPC_URL"
```
