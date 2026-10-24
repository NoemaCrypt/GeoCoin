# Production-тестирование GeoCoin

## Статус и границы утверждения

Локальный четырёхнодовый прогон и весь CTest проходят. Это сильное
доказательство работоспособности текущей ветки, но не математическая гарантия
«100% гарантии» и не сертификат готовности mainnet. Такое утверждение
невозможно получить одним набором тестов: перед публичным запуском остаются
многохостовый soak, длительные генеративные прогоны, проверка отказов диска, кроссплатформенная
матрица и независимый аудит.

Изолированная сеть `degree-load` специально использует отдельный genesis и
фиксированную тривиальную сложность `0x207fffff`. Эти параметры предназначены
только для тестов и не должны появляться в production-манифесте или mainnet
datadir.

## Как запустить полный сценарий

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON
cmake --build cmake-build-release \
  --target geocoin-node geocoin-rpc-miner geocoin-loadnet-stress \
  --parallel
cargo run --release --manifest-path tools/loadnet-stress/Cargo.toml -- \
  --node-binary ./cmake-build-release/geocoin-node \
  --miner-binary ./cmake-build-release/geocoin-rpc-miner \
  --work-dir ./loadnet-runs/full-1000-$(date +%Y%m%d-%H%M%S)
```

Каталог запуска обязан быть новым или пустым. Раннер не перезаписывает
существующий datadir. Значения по умолчанию: 4 ноды, 1 000 адресов, 16 рабочих
потоков, 2 нагрузочных раунда, restart, `SIGKILL`, reorg и набор невалидных
P2P-входов.

Итоговый `PASS` разрешён только если:

- все scenario и coverage gate имеют `true`;
- все неожиданные ошибки равны нулю;
- каждая принятая транзакция подтверждена;
- tip, height и mempool всех нод сошлись;
- все процессы живы и компоненты healthy;
- обычный P2P relay сошёлся без единого `p2p_repair`;
- монитор не увидел отказ вне явно помеченного окна restart/partition;
- в логах нет необъяснённых `WARN`, `ERROR` или `FATAL`;
- p99 транзакций и максимальная RPC-задержка монитора не превышают заданные
  бюджеты;
- секретные файлы закрыты для group/other, а canary и RPC-пароли отсутствуют в
  логах.

Direct block repair оставлен только для сбора дополнительной диагностики. Если
он понадобился, сеть может восстановиться, но вердикт всё равно будет `FAIL`.

## Реально выполняемые live-сценарии

### Genesis, фонд и начальное распределение

- запуск четырёх отдельных archive-нод на случайных loopback-портах;
- проверка отдельного genesis и ruleset сети `degree-load` каждой нодой;
- проверка одного фонда `4-of-4` и всех четырёх управляющих адресов;
- 104 блока с одним CPU-потоком для достижения maturity `100`;
- четыре независимые fan-out транзакции;
- создание, уникальность, сохранение и финансирование 1 000 адресов;
- проверка фиксированной тестовой сложности на живых блоках.

### RPC и разграничение полномочий

- `readonly` читает chain, но не может остановить ноду или изменить wallet;
- `miner` читает mempool, но не получает доступ к wallet;
- отсутствующие и неверные credentials дают HTTP 401;
- parse error не раскрывает credentials или тело-секрет;
- batch ровно на лимите принимается, batch выше лимита отвергается целиком;
- чрезмерная глубина JSON отвергается;
- пустой batch, scalar request и неверная версия JSON-RPC отвергаются;
- ограничение сложности JSON останавливает чрезмерно разветвлённое тело до
  dispatch;
- живой raw-TCP HTTP corpus проверяет `GET` (405), неверный path
  (404), DNS rebinding через `Host` (421) и не-JSON `Content-Type` (415);
- дубли `Content-Length`, `Host` и `Authorization`, а также невалидное
  имя header отвергаются до dispatch;
- дополнительно отвергаются повторный `Content-Type`, пустой `Host`, folded
  header, неоднозначная request line, неизвестная HTTP-версия, слишком длинный
  URI, нечисловой `Content-Length` и неподдерживаемый chunked encoding;
- сочетание `Transfer-Encoding + Content-Length` отвергается;
- заявленное тело в 999 999 999 байт даёт 413 до чтения, а неверная
  авторизация с неполным телом даёт 401 без ожидания body;
- duplicate JSON object keys, включая вложенные, и object-valued JSON-RPC `id`
  отвергаются без вызова команды;
- лимиты RPC-соединений и rate-limit по умолчанию выключены (`=0`); live-сценарий
  держит четыре незавершённых loopback-соединения и проверяет, что пятое
  принимается, а частичные header/body закрываются по deadline;
- после освобождения зависших соединений RPC capacity полностью
  восстанавливается;
- после всего набора невалидных входов нода отвечает healthy и сохраняет
  `Cache-Control: no-store` и `X-Content-Type-Options: nosniff`;
- 32-байтовые RPC-пароли генерируются из `OsRng`, сохраняются в отдельных
  закрытых файлах и проверяются canary-поиском по всем логам.

### Парсинг и построение транзакций

- пустой, нечётный и не-hex raw transaction;
- усечённый header, усечённый witness и лишние trailing bytes;
- неканонический адрес, неверная длина payload и несоответствие `GEO`/`GEOM`
  версии;
- неверный destination через wallet и raw builder;
- пустые inputs/outputs, нулевой output, переполнение `vout` и `locktime`;
- количество inputs и outputs выше consensus-лимита;
- дублированные inputs внутри одной транзакции;
- отсутствующий или уже потраченный UTXO;
- попытка подписать чужим wallet;
- повреждённая Falcon-подпись.

### Денежные и policy-границы

- недостаточный баланс и fee ниже минимального;
- output ниже dust и output ровно на dust-границе;
- сумма outputs больше inputs;
- сумма outputs больше `MAX_MONEY`;
- self-transfer с повторяющимся destination;
- валидный fan-out на 33 outputs;
- ровно 1 000 outputs принимаются и подтверждаются;
- 1 001 input/output отвергается builder/consensus;
- максимальная фактически помещающаяся Falcon-транзакция принимается;
- следующий input, увеличивающий signed transaction выше 100 000 байт,
  отвергается уже после добавления witness;
- `testmempoolaccept` на точном batch-лимите не меняет mempool;
- batch выше лимита отвергается;
- отклонённый preflight не оставляет побочных эффектов.

### Конкурентность и нагрузка

- mempool double-spend;
- одновременная отправка двух конфликтующих spend: ровно один победитель;
- concurrent duplicate storm остаётся идемпотентным;
- single-origin transaction доходит через P2P до всех остальных нод;
- replay подтверждённой граничной валидной транзакции отвергают все ноды;
- отдельно проверяется replay подтверждённой boundary-транзакции;
- два раунда по 1 000 подписанных переводов на 16 потоках;
- случайный recipient pattern и hotspot pattern на один адрес;
- непрерывные samples высоты, tip, mempool, peer count, health и RPC latency;
- bounded backpressure не позволяет клиенту самому перегрузить лимит RPC
  соединений и скрыть поведение node/mempool.

### P2P, sync, storage и reorg

- полный mesh с тремя аутентифицированными encrypted peers у каждой ноды;
- самостоятельный relay транзакций и блоков без RPC repair;
- graceful restart follower с проверкой wallet, UTXO, resync и tip;
- принудительный `SIGKILL` follower после durable writes;
- cold RocksDB/WAL recovery с проверкой прежнего tip, wallet, UTXO и health;
- реальное разделение сети на `2 + 2`, две конкурирующие ветки и выбор ветки с
  большей chainwork;
- откат UTXO/state и возвращение транзакции из orphaned block в mempool;
- повторное подтверждение resurrected transaction на выигравшей ветке;
- четыре независимых pre-auth P2P-случая на разных нодах: unknown message
  type, чужой network magic, неверная фиксированная длина `CHALLENGE` и
  несовпадающая checksum допустимой по размеру рамки;
- каждый P2P-случай повышает точный policy counter, закрывает соединение,
  создаёт temporary ban, а нода остаётся healthy с тремя аутентифицированными
  честными peers;
- после каждого сценария с перезапуском или разделением проверяются одинаковые tip/height, пустые
  mempool и живые процессы.

## Дополнительные автоматические наборы

Полный debug CTest содержит 47 тестов: consensus, transaction, wallet,
chainstate revert, persistence, storage, RPC auth/framing/payload, deterministic
parser fuzz, secure channel tamper/replay/downgrade, peer policy, sync, node
messages, GUI smoke, RPC localnet, P2P-кластеры на 2/3/4 ноды и полный
sidechain-v3 localnet.

### NFT, транзакции и целостность состояния

NFT-матрица покрывает все поддерживаемые переходы `CREATE`, `TRANSFER`,
`BURN` и кадастровый `TRANSFER` с автоматическим удалением объекта:

- пустую, неверную и принадлежащую другому адресу подпись;
- изменение получателя, комиссии, creator, royalty, metadata и данных
  местоположения после подписи;
- расхождение заявленной комиссии `TRANSFER`/`BURN` с фактической разницей
  UTXO-входов и выходов;
- повторный input, отсутствующий/будущий/незрелый/отрицательный UTXO,
  переполнение суммы входов или выходов, отрицательный output, сумму выходов
  выше входов и коллизию ключа нового UTXO;
- полный цикл `CREATE -> TRANSFER -> BURN` внутри одного блока, чистый откат,
  повторное применение того же блока и откат всех трёх переходов при
  некорректной последней транзакции;
- постоянную reservation NFT id после `BURN` и её корректное удаление при
  откате самого `CREATE`;
- отдельное поведение обычных географических NFT и кадастровых участков:
  обычный объект не резервирует кадастровый номер или земельную геометрию;
- ошибки и исключения при записи metadata, owner, uniqueness index, raw
  transaction, удалении входного UTXO и записи выходного UTXO;
- успешную компенсацию с побайтовым равенством UTXO до/после отказа и режим
  `rebuild-required`, если backend не подтвердил восстановление;
- три последовательных холодных открытия реальной RocksDB после `CREATE`,
  `TRANSFER` и `BURN` с проверкой height, tip, владельца, canonical metadata,
  reservation, истории транзакций и полного UTXO snapshot.

Измерение GCC coverage для `src/geocoin/nft/nft_validator.cpp` после этой
матрицы: `329/329` исполняемых исходных строк (`100.00%`), `98.81%` ветвей
выполнены и `62.26%` направлений ветвей взяты хотя бы один раз. Последние два
числа включают сгенерированные компилятором пути стандартной библиотеки и
обработки исключений, поэтому утверждение ограничено точным покрытием строк и
явно перечисленными семантическими условиями, а не обещанием перебрать
бесконечное множество входных байтов.

Для `src/geocoin/core/transaction.cpp` тот же свежий набор даёт `99.72%`
исходных строк и `94.75%` ветвей. Невыполненными остаются защитный throw для
случая, когда строгий canonical parser неожиданно вернул неканоническое
представление, внутренний отказ Falcon signer после уже пройденной проверки
32-байтового ключа и два сгенерированных компилятором exception-only блока.
Эти пути нельзя корректно вызвать публичным входом без специальной подмены
криптографического backend; они сохранены как защита от будущих изменений, а
не скрыты ради формального процента.

Отдельные ChainState-регрессии проверяют сериализацию concurrent
`revert_block`, предварительную проверку undo envelope/interest delta,
отказ `commit_batch`, runtime poison до rebuild и degraded readiness в
`gethealth`. Отдельные process-crash тесты завершают дочерний процесс через
`_exit()` без деструкторов и затем повторно открывают реальные раздельные
RocksDB:

- незавершённая асинхронная запись сохраняет recovery marker и вызывает rebuild;
- переход из IBD в синхронный режим сбрасывает прежние WAL-disabled записи
  обеих баз на диск до удаления marker;
- block body, active tip, UTXO до/после перехода и journal проверяются после
  холодного открытия;
- UTXO незавершённого snapshot, старые undo и epoch-индексы удаляются из
  реальных раздельных RocksDB и не загружаются в память;
- частичная ошибка очистки metadata не удаляет marker и переводит derived state
  в `rebuild-required`.

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j1
```

Критические C++ пути дополнительно проверяются AddressSanitizer и
UndefinedBehaviorSanitizer:

```bash
cmake -S . -B cmake-build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DGEOCOIN_SANITIZER=address-undefined
cmake --build cmake-build-asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir cmake-build-asan --output-on-failure
```

Rust-раннер обязан проходить форматирование, lint без warnings и unit tests:

```bash
cargo fmt --manifest-path tools/loadnet-stress/Cargo.toml -- --check
cargo clippy --manifest-path tools/loadnet-stress/Cargo.toml \
  --all-targets -- -D warnings
cargo test --manifest-path tools/loadnet-stress/Cargo.toml
```

ThreadSanitizer является отдельным обязательным gate на совместимом Linux
runner. Если GCC TSan завершается до `main()` сообщением `unexpected memory
mapping`, результат считается `BLOCKED`, а не `PASS`; тест нужно повторить на
runner без конфликта shadow memory, при необходимости под `setarch x86_64 -R`.

## Последний подтверждённый прогон

Дата: 2026-08-16. Артефакт:
`loadnet-runs/full-1000-integrity-20260816-v2/report.json`.
Его SHA-256:
`7a349eac40b08ce5dbf45b18e54e5958e783172c62cba8d588b36bec400335a0`.

- verdict: `PASS`;
- 4 ноды, 1 000 адресов, 116 добытых блоков;
- 89 уникальных live-сценариев и 26 coverage gate прошли;
- 2 010 valid-attempts, 2 009 приняты, 2 009 подтверждены;
- 65 ожидаемых отказов, 0 неожиданных transaction failures;
- 8 017 наблюдений broadcast-копий;
- latency p50/p95/p99: `32/56/70 ms`;
- `p2p_repairs = 0`;
- SIGKILL recovery node 2: `48 936 ms`;
- 239 monitor samples; 227 отказов внутри явно помеченных
  окон restart/partition и 0 отказов вне них;
- максимальная успешная RPC latency монитора: `1 974 ms` при бюджете
  `10 000 ms`;
- fatal errors: `0`;
- 456 `WARN` и 2 `ERROR` классифицированы как ожидаемые события negative,
  restart и partition сценариев; необъяснённых findings нет.
- Release CTest после последней правки: `47/47`, `162.79 s`; Rust
  fmt/clippy: чисто, unit tests: `7/7`; ASan+UBSan critical subset: `4/4`,
  `20.20 s`, плюс ранее подтверждённый mining/hash backend subset: `3/3`,
  `1.95 s`;
- TSan target собран, но runtime до `main()` заблокирован средой:
  `FATAL: ThreadSanitizer: unexpected memory mapping`.

## Найденные и исправленные дефекты

- address decoder принимал payload с лишними байтами и не связывал prefix с
  version byte;
- mempool policy мог пропустить невалидный output-address через exemption;
- RPC builder сужал 64-битный `locktime` до `uint32_t`;
- размер raw transaction проверялся до Falcon witness, но мог выйти за лимит
  после подписи;
- приватные ключи/промежуточные signing buffers не очищались на всех выходах;
- sync мог запросить потомка выигрышной fork раньше недостающих родителей;
- `GETHEADERS` принимал locator с боковой ветки как anchor активной цепи;
- транзакция, попавшая в mempool во время disconnect/auth, могла больше не
  попасть в relay queue нового peer;
- честный burst `GETDATA`/`BLOCK` при нормальном sync ошибочно попадал под
  rate-limit disconnect;
- relay отправлял INV по одной транзакции и сам создавал INV flood;
- RPC `addnode ... onetry` фактически не обходил накопленный reconnect backoff;
- тестовый раннер маскировал сбой P2P relay через direct repair и всё равно мог
  поставить `PASS`;
- монитор считал все RPC-сбои когда-либо перезапускавшейся ноды ожидаемыми,
  даже если сбой произошёл вне chaos-окна;
- два разных confirmed replay сценария имели одинаковый ID;
- предсказуемые test credentials, короткий cold-start timeout и потеря
  `account_count` в отчёте позднего failure исправлены.
- `ChainState::revert_block` и публичные save/load undo не брали общий
  recursive mutex, что допускало race с apply/query;
- interest-pool underflow при revert проверялся после транзакционных
  мутаций; теперь весь undo envelope и delta проверяются до них;
- отказ revert transaction/`commit_batch` мог оставить частично изменённый
  in-memory state, но нода продолжала выглядеть healthy; теперь runtime state
  poison запрещает новые transition до rebuild, а `gethealth` возвращает
  `rebuild-required`, `ready=false`, `status=degraded`;
- recovery marker мог удаляться после WAL-disabled commit до того, как обе
  раздельные RocksDB действительно оказались на диске; удаление теперь возможно
  только после подтверждённого durable commit;
- первый синхронный commit после выхода из IBD не сбрасывал старые
  WAL-disabled записи; обе базы теперь явно синхронизируются;
- отклонение следующего блока с чистым rollback могло удалить marker от более
  раннего ещё не durable commit; marker теперь сохраняется;
- частичный сбой очистки metadata мог удалить единственный признак
  незавершённого восстановления; marker теперь удаляется последним и только
  после успешной очистки;
- rebuild очищал высоту и специализированные индексы, но мог оставить UTXO,
  undo и старые epoch-индексы незавершённого snapshot; теперь они распознаются
  отдельно, удаляются до продолжения и проверяются после холодного открытия;
- live-runner проверял только JSON-RPC через HTTP-клиент и один P2P malformed
  header; добавлены raw HTTP framing, лимиты сложности/соединений/deadline и
  четыре независимых P2P-сценария;
- первый вариант проверки checksum использовал PING с недопустимым payload и
  попадал в более ранний size-check; тест переведён на допустимый по размеру
  VERSION frame, поэтому теперь однозначно проверяет checksum path.
- удаление NFT из индекса владельца/тегов использовало диапазон от найденного
  элемента до конца и могло удалить ссылки на все последующие NFT; теперь
  удаляется только точное совпадение, а повторяющиеся tag-индексы не создаются;
- геометрия NFT использовала floating-point деление и платформозависимый
  широкий тип; площадь, пересечения рёбер, попадание на границу и кадастровый
  допуск теперь считаются точной 128-битной целочисленной арифметикой;
- NFT-полигон мог быть самопересекающимся при ненулевой shoelace-площади;
  добавлена обязательная проверка простого полигона;
- поля creator, royalty, metadata и Proof-of-Location ранее не были связаны
  одним каноническим подписываемым представлением; теперь оба режима используют
  полный `serialize_for_signing()`, а изменение любого поля ломает подпись;
- дубли тегов, отрицательные кадастровые значения и противоречивые поля
  `has_buildings/building_count` теперь отклоняются до регистрации;
- UPDATE_FOUNDATION_ADDRESS без единой подписи проходил ранний `verify()` и
  отклонялся только state-зависимой проверкой; теперь пустой набор сразу
  отклоняется, а quorum и состав по-прежнему проверяются ChainState;
- загрузчик NFT теперь явно проверен на owner без metadata, metadata без owner,
  неверный адрес владельца, повреждённые байты и несовпадающий canonical id;
  неполные пары удаляются, snapshot помечается требующим rebuild;
- burn удаляет owner/metadata, но сохраняет постоянную reservation NFT id;
  повторный выпуск того же canonical NFT отклоняется, а rollback восстанавливает
  владельца и метаданные.
- standalone NFT validator проверял владельца `TRANSFER`/`BURN`, но не
  криптографическую подпись соответствующего payload; теперь подпись и адрес
  подписанта обязательны на обоих уровнях проверки;
- обычный географический NFT мог ошибочно блокировать кадастровый участок с
  пересекающейся геометрией; сравнение кадастрового номера и границ теперь
  выполняется только между кадастровыми объектами;
- результаты записи owner/metadata NFT игнорировались, а UTXO мог измениться в
  памяти после частичной ошибки backend; persistent-изменения теперь
  подготавливаются до публикации в памяти, компенсируются при отказе и
  переводят ноду в `rebuild-required`, если точное восстановление не
  подтверждено;
- чистый откат удалял UTXO и NFT-состояние, но оставлял raw transaction record,
  из-за чего автономный ChainState мог отклонить повторное применение того же
  корректного блока; raw index обычных/NFT-транзакций теперь удаляется вместе с
  откатываемой транзакцией. Checkpoint-записи намеренно сохраняются как история
  для проверки конфликтующих checkpoint после реорганизации и не считаются
  частью активной цепи публичными lookup.

## Открытые риски до production

1. Весь live-прогон выполнен на одном Linux-хосте и loopback. Нет реальной WAN
   задержки, packet loss, reorder, bandwidth cap, NAT, IPv6 и multi-region
   partition.
2. Cold recovery занял 50,1 секунды даже на небольшой тестовой цепи. Нужны
   профилирование RocksDB/WAL, recovery SLO и проверка на production-объёме.
3. Локальный TSan-прогон wallet/mempool/sync теперь проходит; nightly job
   повторяет его на чистом runner. До release остаётся расширить набор долгими
   многопоточными сценариями ноды и GUI.
4. Для P2P, RPC, raw transaction, block и sidechain proof добавлены отдельные
   инструментальные цели и ежедневный прогон с сохранением накопленных входов.
   Перед release всё ещё нужна более долгая непрерывная кампания.
5. Резкое завершение процесса между записями двух RocksDB и холодное повторное
   открытие теперь проверяются автоматически. Всё ещё не покрыты реальные
   `ENOSPC`, read-only filesystem, битые SST/WAL, power-cut в разных точках
   commit, медленный диск и исчерпание file descriptors.
6. Нет chaos-матрицы clock skew/NTP jump, длительной потери связи, half-open TCP,
   churn и массового трафика с разных IP.
7. Нет 24/72-часового soak с постоянным mining, reorg, restart и миллионами
   транзакций; короткий loopback-тест не выявляет утечки и деградацию со временем.
8. Одинаковый снимок состояния теперь проверяется после 101-блочного replay на
   MemoryStorage, обычном и split RocksDB, после reorg и перезапуска; локально он
   совпал в GCC и Clang. Полная Windows и upgrade/rollback матрица ещё не выполнена.
9. Логи слишком шумные во время ожидаемых negative tests, а некоторые русские
   RPC-сообщения отображаются как `????`. Это мешает production alerting и
   forensic-анализу.
10. Loadnet запускает wallet с явно разрешённым plaintext storage. Для реальных
    средств нужны проверенные encryption-at-rest, backup/restore, rotation,
    hardware-backed key custody и запрет test environment flags.
11. Нужны независимый аудит consensus/state/wallet/RPC/P2P, dependency/SBOM
    review, воспроизводимые подписанные сборки и план ответственного раскрытия.

## Рекомендуемые release-gates

- На каждый PR: full CTest, Rust fmt/clippy/tests, patch whitespace и secret scan.
- Nightly: полный 1 000-account loadnet, sidechain-v3/NFT/multisig localnet,
  ASan/UBSan, TSan и три инструментальных генеративных прогона с артефактами.
- Weekly: более длинная непрерывная кампания, multi-host `tc netem` chaos и
  реальные disk-fault сценарии.
- Перед release candidate: 72-часовой soak, cross-platform consensus replay,
  fresh-node sync, upgrade/rollback, backup/restore и dependency scan.
- Перед mainnet: устранить все critical/high findings независимого аудита,
  выполнить чек-лист `docs/production-mainnet.md` и сохранить подписанный пакет
  отчётов с точным commit hash.
