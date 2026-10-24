# Misc / Control / Util RPC (RU)

**Дата:** 16 апреля 2026
**Источник:** `src/geocoin/rpc/rpc_misc.cpp`

Служебные методы (контроль ноды, утилиты, подписи).

## Методы

- `help ["method"]` — список методов или справка по конкретному методу.
- `getrpcinfo` — машинно-читаемый реестр методов, ролей, категорий, mutability/deprecation metadata.
- `stop` — запрос на остановку сервера (контрольный контракт).
- `getinfo` — краткая сводка (версия, высота, баланс, криптосистема).
- `getnodeinfo` — сводка по ноде, сети, RPC и storage backend.
- `getsyncstatus` — статус синхронизации.
- `gethealth` — компактный health-check.
- `getmetrics` — snapshot внутренних метрик.
- `getstorageinfo` — layout и размеры data-dir/storage (admin diagnostics).
- `getdbstats` — DB/storage статистика (admin diagnostics).
- `getpruninginfo` — pruning режим и состояние.
- `validateaddress <address>` — проверить адрес без раскрытия принадлежности кошельку (read-only).
- `getwalletaddressinfo <address>` — проверить адрес и вернуть `ismine` (admin/wallet capability).

### Подписи (Falcon-512)

- `signmessage <address> <message>` — подписать сообщение ключом адреса (нужен кошелёк).
- `verifysignature <address> <signature_hex> <message>` — проверить подпись.
- `getfalconinfo` — диагностическая информация по Falcon-512 (см. `help "getfalconinfo"`).

### Диагностика PoW

- `powdeterminism` — self-check детерминизма GeoPoW.

Точные параметры и формат результата см. в `help "<method>"`.

## Примеры (curl)

`validateaddress`:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"validateaddress","params":["GEO..."]}' \
  "$RPC_URL"
```

Подпись и проверка подписи:

```bash
# signmessage (нужен загруженный wallet)
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"signmessage","params":["GEO...","hello"]}' \
  "$RPC_URL"

# verifysignature
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":3,"method":"verifysignature","params":["GEO...","<sig-hex>","hello"]}' \
  "$RPC_URL"
```
