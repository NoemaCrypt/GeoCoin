# GeoCoin RPC Reference (RU)

**Дата:** 16 апреля 2026
**Транспорт:** HTTP + JSON-RPC 2.0 (Basic Auth)

Этот раздел — справочник по RPC методам ноды `geocoin-node`, сгруппированный по подсистемам. `degree_node` — совместимый alias.

Во всех страницах ниже есть раздел **Примеры (curl)** с готовыми JSON-RPC запросами.

## Быстрый старт

Минимальный JSON-RPC запрос (пример с `getblockcount`):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" \
  -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  "http://127.0.0.1:$RPC_PORT/"
```

Точный порт зависит от сети (см. вывод `geocoin-node` при старте или `consensus::Params().rpc_port()`):
- mainnet: `3033`
- testnet: `13033`

## Страницы

- Протокол, аутентификация, ошибки: `PROTOCOL_RU.md`
- Stress tests: `STRESS_RU.md`
- Blockchain API (цепочка/блоки/tx query): `BLOCKCHAIN_RU.md`
- Wallet API (локальный wallet, отправка, legacy account методы): `WALLET_RU.md`
- Network API (пиры/соединения/P2P policy stats): `NETWORK_RU.md`
- Mining API (`miner` credentials, block templates, node-local mining): `MINING_RU.md`
- Misc/Control/Util (help/stop/подписи): `MISC_RU.md`

Автогенерация компактного реестра из runtime metadata:

```bash
python3 scripts/generate_rpc_docs.py --url "$RPC_URL" --user "$RPC_USER" --password "$RPC_PASS"
```

## См. также

- Architecture: `../ARCHITECTURE_RU.md`
- Docs index: `../README.md`
