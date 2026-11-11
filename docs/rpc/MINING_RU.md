# Mining RPC (RU)

**Дата:** 16 апреля 2026
**Источник:** `src/geocoin/rpc/rpc_mining.cpp`

## Методы

- `getmininginfo` — сводная информация о майнинге (статус/параметры/высота/сложность).
- `getmempoolinfo` — read-only статистика mempool для выбора транзакций.
- `getnetworkhashps` — оценка хешрейта сети.
- `getblocktemplate {"address":"..."}` — шаблон блока для внешнего майнера.
- `submitblock <hex>` — отправить найденный блок.
- `setgenerate <true|false> [threads]` — включить/выключить майнинг в ноде.

`getmininginfo`, `getmempoolinfo`, `getnetworkhashps`, `getblocktemplate`, `submitblock` требуют отдельные `miner` RPC credentials. `setgenerate` — локальное управление CPU mining внутри ноды, требует `admin` и wallet.

Точные параметры и формат результата см. в `help "<method>"`.

`getblocktemplate.geography` описывает ту же точку WGS 84, чья ячейка
записана в `geo_tag` внутри `blockhex`. Формат определён
[контрактом координат версии 1](PROTOCOL_RU.md#географические-координаты-контракт-версии-1).
Полная точка пересчитывается из контекста родителя шаблона.

## Примеры (curl)

Информация о майнинге:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' \
  "$RPC_URL"
```

Включить майнинг (пример, 4 потока):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"setgenerate","params":[true,4]}' \
  "$RPC_URL"
```

Шаблон для внешнего майнера:

```bash
curl -sS -u "$RPC_MINER_USER:$RPC_MINER_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":3,"method":"getblocktemplate","params":[{"address":"GEO..."}]}' \
  "$RPC_URL"
```
