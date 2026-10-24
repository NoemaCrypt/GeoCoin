# Wallet RPC (RU)

**Дата:** 16 апреля 2026
**Источник:** `src/geocoin/rpc/rpc_wallet.cpp`

Методы этого раздела управляют локальным кошельком и формируют/отправляют транзакции. Публичные chain/mempool/NFT getters вынесены в категории `blockchain`, `mempool`, `nft` в `getrpcinfo`, даже если исторически описаны рядом с wallet workflows.

## 1) Кошелёк и адреса

- `createwallet` — создать/инициализировать кошелёк.
- `getnewaddress` — новый адрес.
- `importprivkey <hex> [label] [password] [rescan=true] [start_height=0]` — импорт приватного ключа и, по умолчанию, восстановление его истории из активной цепи.
- `dumpprivkey <address>` — экспорт приватного ключа.
- `getaddressesbyaccount <account>` — legacy account-style список адресов; для новых клиентов помечен как deprecated в `getrpcinfo`.

`importprivkey` возвращает `rescan_requested`, `rescan_complete`, `rescan_status` и
`next_height`. Статусы `read-error`, `history-pruned` и `chain-changed` означают,
что ключ сохранён, но история пока неполна; после восстановления доступа к телам
блоков повторите `importprivkey` с тем же ключом и `start_height`, равным
`next_height`. Повторный импорт того же ключа идемпотентен. Передача
`rescan=false` явно отключает восстановление истории.

## 2) Балансы и UTXO

- `getbalance` — общий баланс кошелька.
- `listaddressbalances` — баланс по адресам кошелька.
- `listunspent` — UTXO кошелька.
- `getaddressutxos <address>` — UTXO по адресу (`readonly`, категория `blockchain`).
- `getutxos <address>` — legacy single-address alias для `getaddressutxos`, deprecated.
- `getfeerate` — текущая оценка fee/rate (`readonly`, категория `mempool`).
- `getfundbalance` — баланс фонда/спец-аккаунта (`readonly`, категория `blockchain`).

## 3) Отправка

- `sendtoaddress <address> <amount> ...` — создать/подписать/отправить транзакцию.
- `sendrawtransaction <hex>` — отправить raw транзакцию (проверка + mempool).
- `verifyrawtransaction <hex>` — проверить транзакцию без добавления в mempool (`readonly`, категория `mempool`).
- `listtransactions [account] [count] [skip]` — legacy account-style история транзакций кошелька; deprecated для новых клиентов.

### Raw transaction builder

- `createrawtransaction <inputs> <outputs> [locktime]` — собрать unsigned STANDARD raw transaction. В outputs поля `amount`/`value` задаются в GC; `value_angles` задаёт точное значение в angles.
- `decoderawtransaction <hex>` — декодировать raw transaction в JSON.
- `signrawtransactionwithwallet <hex>` — подписать raw transaction локальным wallet, если все inputs принадлежат одному локальному адресу.

Рекомендуемый pipeline для внешних клиентов: `createrawtransaction` → `signrawtransactionwithwallet` → `testmempoolaccept`/`verifyrawtransaction` → `sendrawtransaction`.

`locktime = 0` отключает задержку. Значения меньше `500000000` задают высоту,
остальные — Unix-время заголовка блока. Порог должен быть строго пройден:
например, `locktime = 100` допускает включение начиная с блока 101.
При ненулевом `locktime` пропущенный `input.sequence` становится `4294967294`;
явные значения сохраняются. Если **все** входы имеют `sequence = 4294967295`,
ограничение отключено. Прочие значения включают только абсолютный `locktime`,
без относительных задержек. Декодирование возвращает `lock_time` и `sequence`
каждого входа; подписание их не меняет.

Подробное декодирование также возвращает `inputs[].script_sig_hex`,
`outputs[].script_pubkey_hex`, `witness_version` и `witnesses[]` с полями
`public_key_hash`, `public_key_hex`, `signature_hex`. У обычных входов показан
`signature_hash`, у ANGLE_COMMITMENT — подпись тела `signature_hex`.
Пустая hex-строка означает отсутствие байтов в самой транзакции; декодер
не подставляет ключи из базы. Пустые witness-записи unsigned-транзакции показаны
так же, как они кодируются в raw.

`memo` хранится только как локальная заметка истории кошелька и не входит
в raw/txid или результат `decoderawtransaction`. Поля другого типа транзакции
не могут использоваться как дополнительные метаданные: заполненный неактивный
payload отклоняется до подписания или сохранения.

GeoCoin использует адресную UTXO-модель: `script_sig` и `script_pubkey` —
неисполняемые байтовые поля, входящие в тело транзакции. В coinbase первые
8 байт `script_sig` содержат высоту; подписи обычных входов находятся в witness.
Лимит каждого из двух байтовых полей — 10000 байт. Конструкторы sidechain payload
соблюдают его и при `validate=false`.

Приём в mempool проверяет следующую высоту и текущее время узла; до срока он
отклоняет транзакцию с причиной `non-final`. Подписанный raw можно подготовить
заранее и отправить после срока. Сборка блока повторно проверяет его конкретное
время. Это время цепи с сетевым допуском заголовка, а не точное расписание по
часам ОС. Полный контракт: [ADR 0005](../../adr/0005-absolute-transaction-finality.md).

## 4) NFT

- `getnft <nft_id>` — получить NFT по id (`readonly`, категория `nft`).
- `listnftsbyowner <address>` — список NFT по владельцу (`readonly`, категория `nft`).

Workflow «payload → finalize»:
- `createnftcreatepayload`
- `finalizenftcreate`
- `createnfttransferpayload`
- `finalizenfttransfer`
- `createnftburnpayload`
- `finalizenftburn`

Метаданные NFT используют общий разбор и форматирование для подготовки подписи,
локального `createnft`, `getnft` и декодирования транзакций. `content_hash` обязателен:
это 32 байта в hex. `polygon` содержит от 3 до `MAX_NFT_POINTS` точек в целых
микроградусах — объектов `{lat_udeg,lon_udeg}` или пар `[lat_udeg,lon_udeg]`.
Широта ограничена ±90 000 000, долгота — ±180 000 000. Старые поля треугольника
`lat1`/`lon1` не принимаются.

Необязательные поля метаданных: `name`, `description`, `image_uri`, `area_bits`,
`created_at`, `block_height`, `block_hash`, `location_name`, `creator`, `tags`,
`cadastral`. Переданные хеши и коллекции проверяются; некорректные значения
не отбрасываются молча. Денежные поля кадастра — только `market_value_angles` и
`cadastral_value_angles`, неотрицательные целые значения в angles. Остальные поля
кадастра: `cadastral_number`, `owner_name`, `land_category`, `permitted_use`,
`area_sqm_official`, `ownership_document_hash`, `technical_plan_hash`, `photo_hashes`,
`last_valuation_date`, `restrictions`, `has_buildings`, `building_count`.
Неизвестные поля метаданных и кадастра отклоняются. `getnft.metadata` включает
`content_hash` и допускает обратный разбор без потери сериализуемых полей.

Для воспроизведения внешней подписи явно задавайте все относящиеся к ней поля:
пропущенные время создания, контекст блока и создатель в `createnftcreatepayload`
получают нулевые/пустые значения, а локальный `createnft` подставляет текущий контекст.
`royalty_percent` — целое число базисных пунктов от `0` до `10000`.

## 5) Sidechain

### Genesis

Генезис сайдчейна находится в blockchain-разделе:
- `getsidechaingenesis`
- `createsidechaingenesis`

### Регистрация / Mint / Redeem / Slash / Lock / Top-up / Spend / Unlock

В кошельке используется общий паттерн:

1) `create...payload` — собрать payload и (обычно) выполнить `validate=true` по умолчанию.
2) подпись/подписи (в зависимости от типа)
3) `finalize...` — собрать финальный `tx_hex`/`txid`. Эти методы не отправляют транзакцию в mempool; отправка выполняется отдельным `sendrawtransaction` или специализированным `send...package`.

Набор методов:

- `createsidechainissuepayload` / `finalizesidechainissue`
- `createaccountingsidechainpayload` / `finalizeaccountingsidechain`
- `createsidechainmintpayload` / `finalizesidechainmint`
- `createsidechainredeempayload` / `finalizesidechainredeem`
- `createsidechainslashpayload` / `finalizesidechainslash`
- `createsidechainlockpayload` / `finalizesidechainlock`
- `createsidechaintopuppayload` / `finalizesidechaintopup`
- `createsidechainspendpayload` / `finalizesidechainspend`
- `createsidechainunlockpayload` / `finalizesidechainunlock`

### Checkpoints

- `createsidechaincheckpointpayload` / `finalizesidechaincheckpoint`
- `createcheckpointproofv2`
- `createaccountingcheckpointproofv1`
- `createbatchsidechaincheckpointpayload` / `finalizebatchsidechaincheckpoint`
- `createsidechaincommitteeupdatepayload` / `finalizesidechaincommitteeupdate`
- `createsidechaindecommissionpayload` / `finalizesidechaindecommission`

Financial sidechain использует checkpoint policy/version `2/2` и может выполнять
mint/redeem. Accounting sidechain использует `3/1`, фиксирует `namespace` и
`schema_hash`, но не имеет токеномики. После decommission реестр остается
slashable еще `144` блока; затем `UNLOCK_DEPOSIT` удаляет реестр и депозит.

## 6) Multisig / Delegation

- `sendmultisigpackage` — отправка multisig пакета.
- `registermultisig` — регистрация multisig (см. `help "registermultisig"`).
- `createdelegationproof` — создать delegation proof.
- `createrevokedelegation` — создать revoke delegation.
- `senddelegationpackage` — отправить delegation пакет.
- `sendrevokepackage` — отправить revoke пакет.

## Примечания

- Для точных параметров используйте `help "<method>"` — в коде help-строки считаются «источником истины».

## Примеры (curl)

Получить баланс:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getbalance","params":[]}' \
  "$RPC_URL"
```

Отправить raw транзакцию:

```bash
RAW_TX_HEX="<hex>"

# Быстрая проверка без добавления в mempool:
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"verifyrawtransaction\",\"params\":[\"$RAW_TX_HEX\"]}" \
  "$RPC_URL"

# Отправка в mempool (если валидно):
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"sendrawtransaction\",\"params\":[\"$RAW_TX_HEX\"]}" \
  "$RPC_URL"
```
