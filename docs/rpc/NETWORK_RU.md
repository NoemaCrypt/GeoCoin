# Network RPC (RU)

**Дата:** 16 апреля 2026
**Источник:** `src/geocoin/rpc/rpc_network.cpp`

Методы для мониторинга p2p и управления пирингом.

## Методы

- `getpeerinfo` — список подключённых пиров и их состояние.
- `getconnectioncount` — количество подключений.
- `getaddednodeinfo` — persistent `addnode` targets и состояние подключения.
- `getp2ppolicystats` — статистика policy-отклонений P2P и временных банов.
- `listbans` — активные manual bans.
- `addnode <ip:port> <add|remove|onetry>` — добавить/удалить пир или попробовать подключиться один раз.
- `disconnectnode <ip[:port]>` — разорвать соединение с пиром.
- `setban <ip|subnet> <add|remove> [bantime]` — добавить/удалить manual ban.
- `clearbans` — очистить manual bans.

Для `readonly` доступны только агрегаты `getconnectioncount` и
`getp2ppolicystats`. Детали адресов пиров и банов из `getpeerinfo`,
`getaddednodeinfo`, `listbans`, а также `getconnectiondiagnostics` требуют
`admin`. Методы управления пирингом и ban-list тоже требуют `admin` и помечены
в `getrpcinfo` как `mutates=true`.

Точные параметры и формат результата см. в `help "<method>"`.

## Примеры (curl)

Список пиров (admin credential):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getpeerinfo","params":[]}' \
  "$RPC_URL"
```

Добавить пир (пример):

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"addnode","params":["1.2.3.4:3030","add"]}' \
  "$RPC_URL"
```
