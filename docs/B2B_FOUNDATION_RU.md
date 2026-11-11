# B2B Foundation v1

Этот документ фиксирует contract между будущими B2B-продуктами GeoCoin и L1.
L1 хранит идентификаторы, неизменяемые хеши, статусы, корпоративные quorum-policy
и settlement/checkpoint события. Персональные данные, терминальная логика и
приватные ключи HSM остаются в продуктовых сервисах.

Необратимые решения: `adr/0001`–`adr/0004`. Карты на L1 не имеют балансов;
recovery эмитента не восстанавливает sidechain-депозит.

## 0. Post-quantum profile

Критические подписи GeoCoin используют Falcon-512. ECDSA, EdDSA и secp256k1 не
являются поддерживаемыми схемами подписания для transaction/account layer.

P2P wire protocol v2 требует hybrid key establishment на базе ML-KEM-768 и
X25519. X25519 используется только как часть hybrid-схемы вместе с ML-KEM-768 и
никогда не принимается как единственный механизм установления ключа. Plaintext,
classical-only и downgrade-варианты соединения отклоняются.

## 1. Корпоративная issuer policy

`geocoin::b2b::CorporateIssuerPolicy` задаёт четыре независимых M-of-N роли:

- `mint` — выпуск карты, токена или учётной единицы;
- `freeze` — suspend/revoke и экстренная блокировка;
- `operations` — обычные операционные действия, включая возобновление;
- `recovery` — смена policy после компрометации или потери ключей.

Список адресов каждой роли должен быть лексикографически отсортирован, не
содержать дубликатов и состоять из валидных GEO-адресов. Policy id —
`SHA-256("geocoin:b2b:issuer-policy:v1" || canonical_policy)`.

Авторизация подписывается над:

`"geocoin:b2b:authorization:v1" || policy_id || role || operation || object_id || nonce || expires_at`

`nonce` уникален в пределах policy и хранится продуктовым backend. `expires_at`
обязателен для сетевых запросов и ограничивает повторное использование
авторизации. Подписи создаются ключами HSM либо изолированными signing-сервисами;
приватные ключи не передаются в web/mobile backend.

L1 этот payload не проверяет и nonce не хранит. Консенсус считает quorum по
адресам подписанных UTXO-входов, входящих в нужную роль (ADR 0001).

`CARD_PROGRAM_DEFINE` поддерживает `corporate_policy.version=1` с четырьмя
независимыми M-of-N наборами GEO-адресов. L1 требует recovery quorum для
создания программы и `CARD_UPDATE_POLICY`, mint quorum для `CARD_ISSUE`, freeze
quorum для suspend/revoke и operations quorum для reactivation. Каждая
авторизующая сторона подписывает один из входов транзакции своим HSM-ключом.

`CARD_UPDATE_POLICY` сохраняет `issuer` и `scope_id` неизменными, а recovery
quorum старой policy может заменить все роли и compliance commitment. Это
аварийная смена эмитентской авторизации: UID и статусы карт остаются на
программе. Это не восстановление балансов и не recovery sidechain-депозита.

## 2. Лимиты и комплаенс

Универсальные consensus-инварианты в L1: корректная подпись нужной роли,
уникальность UID/public key, допустимый переход статуса, лимит collateral,
лимит выпуска sidechain и `max_active_cards` для card program. В
`compliance_policy_hash` якорится версия внешней политики. Политики клиента
проверяет B2B backend:

- дневные/месячные лимиты и velocity;
- AML/KYC/risk score и санкционные списки;
- лимиты по терминалу, сотруднику, карте, merchant и региону;
- case management и ручное разрешение исключений.

Денежные/velocity-лимиты не могут быть L1-инвариантом, пока L1 не исполняет
карточные платежи: их должен обеспечивать sidechain или authorization backend.
Каждое решение backend хранит policy id, policy version, decision id, timestamp,
authorizer set и metadata hash. В L1 якорится хеш решения, а не персональные
данные.

## 3. Канонические metadata hash

Payload метаданных сериализуется до хеширования либо RFC 8785 JSON Canonicalization
Scheme, либо deterministic CBOR. Формат выбирается в schema registry и не
смешивается внутри одной schema id.

`metadata_hash = SHA-256("geocoin:b2b:metadata:v1" || network_id || tenant_id || object_type || schema_id || schema_version || canonical_payload)`.

Все строки — UTF-8 с CompactSize длиной; `schema_version` — uint32 LE; payload
length-prefixed. `network_id` / `tenant_id` / `object_type` нельзя подменять
одним `schema_id` (ADR 0002).

`schema_id` имеет reverse-DNS вид, например `com.geocoin.card.metadata`.
Изменение смысла поля требует новую schema version; ранее опубликованные версии
не меняются. Бинарные документы, фото и персональные данные хранятся off-chain,
а в canonical payload включаются их content hash и encryption/key-reference.

## 4. Namespace для scope id

`scope_id = SHA-256("geocoin:b2b:scope:v1" || network_id || tenant_id || product_id || scope_name)`.

Каждая строка кодируется UTF-8 с CompactSize длиной. `tenant_id` — стабильный,
непереиспользуемый идентификатор организации; не используйте в нём ФИО, email,
номер карты или иной персональный идентификатор. `scope_id` допускается в
CARD-программах и может совпадать с id sidechain только если это явно описано
в product schema.

## 5. Sidechain checkpoint v2

Для финансового L1: `policy_id=2`, `version=2`. Для accounting: `policy_id=3`,
`version=1`. Batch item содержит `sidechain_id`, `checkpoint`, sidechain height,
fee, committee id, M-of-N signatures и proof.

Это федеративная модель: L1 проверяет структуру proof, ограничение supply,
монотонность checkpoint и подписи зарегистрированного комитета. Financial v2
доказывает, что supply leaf входит в заявленный `state_root`; не доказывает
остальные листья и корректность VM. Конфликт на одной sidechain height с двумя
разными checkpoint hash — основание для `SLASH_DEPOSIT`, а не выбор «истинного»
стейта. Финальность транзакции checkpoint на L1 не равна финальности исполнения
sidechain (ADR 0003).

L1 задаёт экономику чекпоинта: минимум раз в 5 блоков L1, мягкое окно 30 блоков
с повышением min fee, оплата с депозита. Каждый sidechain product отдельно
определяет:

- алгоритм finality и минимальное число подтверждений;
- state schema и способ получения state root;
- процедуру спора, slash и ротации комитета;
- период checkpoint, retention, RPO/RTO и бюджет комиссий;
- независимый мониторинг supply, divergence и недоступности подписантов.

Изменение proof semantics или добавление нового checkpoint policy — отдельный
consensus upgrade. Обычные B2B-функции поверх текущего state root не требуют
изменения L1.
