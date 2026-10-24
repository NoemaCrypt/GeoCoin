# RPC protocol (RU)

**Дата:** 12 сентября 2026

## Транспорт и формат

GeoCoin реализует **JSON-RPC 2.0 поверх HTTP**:

- HTTP метод: `POST`
- Тело: объект запроса или пачка до 10 запросов.
- Успех: `{"jsonrpc":"2.0","id":1,"result":...}`.
- Ошибка: `{"jsonrpc":"2.0","id":1,"error":{"code":...,"message":...}}`.

Ответ содержит ровно одно из полей `result` и `error`, согласно
[контракту ответов JSON-RPC 2.0](https://www.jsonrpc.org/specification#response_object).
`getrpcinfo.api_version` равен `"1"`. До запуска проекта используется версия API 1;
версия 2 предусмотрена для будущего пересмотра API после запуска.
Клиент проверяет наличие `error`;
успешный ответ больше не содержит `error: null`.

Корректный запрос без `id` — уведомление: метод выполняется без JSON-ответа,
в том числе при ошибке выполнения. HTTP возвращает `204 No Content` для отдельного
уведомления или пачки только из уведомлений. В смешанной пачке уведомления
пропускаются в ответе. Явный `"id":null` получает ответ. Ошибки прогрева сервера
сохраняют идентификаторы и соблюдают эти же правила пачек и уведомлений.

`params` принимает массив или объект с именами из `getrpcinfo.param_names`.
Пропущенные необязательные именованные параметры используют значения по умолчанию,
даже если задан более поздний параметр. Пустой `{}` допустим у метода без аргументов.
Пропуск обязательного аргумента и неизвестное имя параметра приводят к ошибке.
Входные расширения: отсутствие `jsonrpc` допускается, а `params:null` означает
отсутствие аргументов; ответы всегда содержат `jsonrpc:"2.0"`. Некорректный объект
запроса получает ошибку с `id:null`, если корректный идентификатор не задан.

Целочисленные параметры отклоняют boolean, дробные JSON-числа, отрицательные
значения для беззнаковых полей и выход за диапазон целевого типа. Усечения и
переполнения при преобразовании нет. Для сумм в GC используется отдельный точный
разбор денежных значений.

Реализация: `src/geocoin/rpc/rpc_protocol.cpp` (HTTP),
`src/geocoin/rpc/rpc_server.cpp` (жизненный цикл и dispatch) и
`src/geocoin/rpc/rpc_server_security.cpp` (аутентификация и rate limit).

## Аутентификация (обязательно)

Используется HTTP Basic Auth (user/pass).

Важно: если `rpc_user` или `rpc_password` пустые, запуск сервера
отклоняется (fail-closed). См. `RPCServer::start` и `RPCServer::authenticate`.

Для ошибок аутентификации действует экспоненциальная задержка. Лимит запросов
по умолчанию выключен (`rpc-rate-limit=0`); при ненулевом бюджете превышение
даёт `429 Too Many Requests`.

## Пример запроса

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "getblockhash",
  "params": [100]
}
```

## Примеры (curl)

Рекомендуемые переменные окружения:

```bash
export RPC_URL="http://127.0.0.1:8778/"
export RPC_USER="user"
export RPC_PASS="pass"
```

Порты по умолчанию зависят от сети и печатаются нодой при старте (или задаются через `--rpc-port`):
- mainnet (`degree`): `3033`
- testnet (`degree-test`): `13033`

Вызов `help`:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"help","params":[]}' \
  "$RPC_URL"
```

Справка по конкретному методу:

```bash
curl -sS -u "$RPC_USER:$RPC_PASS" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"help","params":["getblock"]}' \
  "$RPC_URL"
```

## Ошибки

Код и сообщение возвращаются в `error`:

```json
{
  "jsonrpc": "2.0",
  "error": { "code": -32602, "message": "..." },
  "id": 1
}
```

Список кодов: `include/geocoin/rpc/rpc_types.hpp` (`RPCErrorCode`).

## Help / интроспекция

- `help` — список методов или справка по конкретному методу.
  - `help` → выдаёт методы, сгруппированные по `category`
  - `help "getblock"` → печатает точную сигнатуру/поля из `RPCCommand::help`
- `getrpcinfo` — машинно-читаемый реестр методов: `api_version`, `name`, `category`, `role`, `capabilities`, `requires_wallet`, `param_names`, `mutates`, `side_effect_free`, `stability`, `since`, `deprecated`, `deprecation`, `side_effects`.

Документируйте интеграции через `help`, если нужен «точный» контракт параметров.

## Роли RPC

RPC credentials разделены по ролям:

- `admin` — wallet/admin операции и read-only методы.
- `readonly` — только методы с ролью `readonly`.
- `miner` — только внешний майнинг (`getblocktemplate`, `submitblock` и miner telemetry).

Роли не являются полной иерархией: `admin` не вызывает miner-only методы, а `miner` не вызывает обычные readonly методы. Проверяйте поле `role` через `getrpcinfo`.
