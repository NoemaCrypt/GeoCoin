# Wallet RPC (RU)

**Дата:** 16 апреля 2026
**Источник:** `src/geocoin/rpc/rpc_wallet.cpp`

Методы этого раздела управляют локальным кошельком и формируют/отправляют транзакции. Публичные chain/mempool/NFT getters вынесены в категории `blockchain`, `mempool`, `nft` в `getrpcinfo`, даже если исторически описаны рядом с wallet workflows.


Биржевой RPC API 1 включает `gettransaction`, `listsinceblock`, `getwalletinfo`,
`estimatesmartfee`, `encryptwallet`, `walletpassphrase`, `walletlock`,
`walletpassphrasechange`, `backupwallet`, `restorewallet` и `rescanblockchain`.
Точные единицы, реорганизации, блокировка и восстановление описаны
в [инструкции интеграции](../exchange-integration.txt).

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
- `listtransactions [account] [count] [skip]` — история по выходам со знаковыми суммами; для опроса используйте `listsinceblock`.

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

Подготовка подписи, `createnft`, `getnft` и декодированные транзакции используют
общий разбор метаданных. Обязательный `content_hash` — 32 байта в hex.
Обязательная `geometry` имеет следующий собственный формат:

```json
{"parts":[{"exterior":[
  {"lat_ndeg":50450000000,"lon_ndeg":30520000000},
  {"lat_ndeg":50450100000,"lon_ndeg":30520000000},
  {"lat_ndeg":50450000000,"lon_ndeg":30520100000}
],"holes":[]}]}
```

`parts` содержит 1..16 частей участка. У каждой — внешний контур `exterior` и
необязательные отверстия `holes`. Всего допускается до 64 колец и 256 вершин,
не менее трёх вершин в кольце. Координаты — знаковые int64 в **наноградусах**:
широта ±90 000 000 000, долгота ±180 000 000 000. Замыкающая вершина в собственном
формате не повторяется. Именованные оси сохраняют порядок широта/долгота EPSG:4326;
GeoJSON использует долготу/широту в градусах. Старые `polygon` и поля треугольника
не принимаются.

Рёбра — единственные кратчайшие геодезические WGS 84; кольцо задаёт меньшую
область. Отверстия должны строго лежать внутри внешнего контура, без касаний и
наложений. Внутренние области частей не пересекаются. Поддерживаются полюса и
линия смены дат. Самопересечения, самокасания, повторные физические позиции и
неоднозначные антиподальные рёбра отклоняются с указанием части, кольца, вершины
или пары рёбер. Канонизация нормализует ±180° и долготы полюсов, направляет внешний
обход против часовой стрелки, отверстия — по часовой, начинает кольцо с минимальной
вершины и сортирует отверстия/части. Эквивалентная перестановка обхода даёт те же
подписываемые байты геометрии. Неканоническое кодирование в сети отклоняется.

Необязательные метаданные: `name`, `description`, `image_uri`, `area_bits`,
`created_at`, `block_height`, `block_hash`, `location_name`, `creator`, `tags`,
`cadastral`. Поля кадастра: `cadastral_number`, `owner_name`, `land_category`,
`permitted_use`, `area_sqcm_official`, `ownership_document_hash`, `technical_plan_hash`,
`photo_hashes`, `market_value_angles`, `cadastral_value_angles`, `last_valuation_date`,
`restrictions`, `has_buildings`, `building_count`. Неизвестные поля отклоняются.
Денежные поля — неотрицательные целые angles. `getnft.metadata` допускает обратный
разбор без потери байтов. `area_bits` остаётся подписываемым непрозрачным полем.

Для воспроизведения внешней подписи явно задавайте все относящиеся к ней поля:
пропущенные время создания, контекст блока и создатель в `createnftcreatepayload`
получают нулевые/пустые значения, а локальный `createnft` подставляет текущий контекст.
`royalty_percent` — целое число базисных пунктов от `0` до `10000`.

Площадь, периметр, расстояния до целых рёбер и пересечения вычисляет
**GeographicLib 2.3.0**, коммит `849284b9620b2ee4f0b9841b6822c67f9d0fa0c2`.
`area_sqcm_official` хранит **см²**: для м² делить на 10 000. Геодезическая площадь
округляется до см² перед сравнением с прежним допуском 10%. Площадь вычитает
отверстия и складывает части; периметр включает все контуры. Расстояние равно нулю
внутри заполненной области и на границе, положительно внутри отверстия.
Производный пространственный индекс и кеш подготовленной геометрии 32 МиБ ускоряют
поиск. В базе геометрия хранится только в метаданных. `getnft.geography` и
`nft_create.geography` содержат вычисленные показатели вне подписываемого metadata.

`analyzegeometry <geometry> (tolerance_m)` — метод чтения для собственного формата
или RFC 7946 Polygon/MultiPolygon/Feature/коллекций. Возвращает `valid`, каноническую
геометрию, показатели и диагностику с индексами от нуля; переданный допуск также
запрашивает GeoJSON для отображения. Импорт адаптивно приближает прямые отрезки
GeoJSON в долготе/широте геодезическими рёбрами: по умолчанию 1 м, диапазон
0,001..1000 м. Неразрезанные скачки долготы более 180° отклоняются; их нужно
разделить по RFC 7946. При превышении 256 вершин требуется увеличить допуск.

`getnftgeojson <nft_id> (tolerance_m=1)` экспортирует Feature. Геодезические рёбра
адаптивно уплотняются и разрезаются на ±180° с сохранением отверстий и полярных
областей. Boost.Geometry применяется только для отображения, вне консенсуса.
`properties.native_geometry` позволяет точно восстановить собственные байты,
`properties.display_tolerance_m` указывает допуск отображения. Обратный импорт
экранного GeoJSON даёт приближение; для сохранения NFT ID явно используйте native.
Qt принимает те же форматы или строки широта,долгота с девятью знаками, показывает
диагностику, контуры, отверстия, площадь и периметр до отправки.

`CADASTRAL_DOC` содержит полный `nft_id` активного кадастрового NFT. `owner_address`
должен совпадать с текущим владельцем, а транзакция — тратить подписанный им вход.
Необязательный кадастровый номер должен совпадать с метаданными. Геометка блока
не ограничивает участок. Подбор в блок учитывает предшествующие сжигания и передачи
NFT. Proof-of-Location остаётся выключенным. Разрешение хранения и допуск проекта
не удостоверяют точность межевания или право собственности.

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
