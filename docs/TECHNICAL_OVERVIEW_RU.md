# GeoCoin — техническая документация (обзор)

**Дата:** 16 апреля 2026

Этот документ — «точка входа» для разработчиков: что где лежит, какие подсистемы есть в ноде, и какие форматы/контракты считаются важными.

Архитектурная страница с основным data-flow и привязкой к слоям: `ARCHITECTURE_RU.md`.
Справочник RPC по разделам (RU/EN): `rpc/README_RU.md`, `rpc/README_EN.md`.

## Состав репозитория

- Корень репозитория — основной C++ проект (нода, консенсус, хранилище, RPC, Qt-кошелёк).
- `libgeo/` — C SDK для внешних клиентов.

Быстрый старт по сборке и запуску: `build-and-run.md`.

## Структура кода ноды (C++)

Основные модули находятся в `../src/geocoin/`:

- `../src/geocoin/core/` — базовые структуры (блоки/транзакции/серилизация).
- `../src/geocoin/consensus/` — параметры и правила консенсуса.
- `../src/geocoin/chain/` — работа с цепочкой (индексы, выбор лучшей ветки, ссылки).
- `../src/geocoin/state/` — `ChainState`, применение блоков, UTXO/аккаунтинг (в зависимости от модели), фоновые задачи.
- `../src/geocoin/storage/` — хранение блоков/метаданных (RocksDB), включая witness pruning.
- `../src/geocoin/network/` — p2p, протокол, синхронизация.
- `../src/geocoin/rpc/` — RPC поверхность ноды и кошелька.
- `../src/geocoin/crypto/` — подписи и криптопримитивы (включая Falcon-512 через liboqs/oqs/pqclean).
- `../src/geocoin/node/` — сборка подсистем в runnable daemon (`geocoin-node`; `degree_node` — совместимый alias).
- `../src/geocoin/wallet/` — кошелёк (ключи, аккаунты, транзакции, интеграция с RPC/GUI).
- `../src/geocoin/nft/` — NFT-логика (если включена манифестом активации).

## Ключевые форматы и контракты

### Формат подписи Falcon-512

Формат упаковки подписи и проверки размеров реализованы в:

- `../src/geocoin/crypto/signatures.cpp`

Реализация: `../src/geocoin/crypto/signatures.cpp`.

### Witness pruning

Механика удаления witness-данных для старых блоков (pruned-ноды), отдельный payload и проверка целостности:

- `../src/geocoin/storage/block_storage.cpp`

Реализация: `../src/geocoin/storage/block_storage.cpp`, `../src/geocoin/state/state.cpp`.

## Сборка, запуск, эксплуатация

- Сборка/запуск: `build-and-run.md`
- Production checklist: `production-mainnet.md`
- Seed runbook: `seed-node-runbook.md`

## Связанные документы

- Возможности блокчейна (RU): `BLOCKCHAIN_CAPABILITIES_RU.md`
- DAA и границы PoW-модели безопасности: `BLOCKCHAIN_CAPABILITIES_RU.md`
- SDK (`libgeo`): `../../libgeo/README.md`
