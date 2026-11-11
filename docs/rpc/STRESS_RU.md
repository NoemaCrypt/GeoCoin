# RPC stress tests (RU)

**Дата:** 16 апреля 2026

В репозитории есть лёгкий нагрузочный тест RPC без дополнительных зависимостей: `scripts/rpc_stress.sh`.
Также есть набор «безопасных» read-only сценариев: `scripts/rpc_stress_suite.sh`.

## Что измеряет

- число успешных/ошибочных запросов (HTTP status)
- пропускную способность (req/s по wall time)
- latency percentiles p50/p95/p99 из `curl time_total` (только по успешным ответам)

## Как запускать

1) Запустите ноду с включённым RPC и заданными `rpc-user/rpc-pass` (см. `docs/build-and-run.md`).
2) Запустите стресс-тест:

```bash
chmod +x scripts/rpc_stress.sh
chmod +x scripts/rpc_stress_suite.sh

export RPC_URL="http://127.0.0.1:13033/"
export RPC_USER="user"
export RPC_PASS="pass"

./scripts/rpc_stress.sh --url "$RPC_URL" \
  --method getblockcount --params '[]' \
  --requests 5000 --concurrency 50 --warmup 200
```

Запуск набора read-only сценариев:

```bash
./scripts/rpc_stress_suite.sh --url "$RPC_URL" \
  --user "$RPC_USER" --pass "$RPC_PASS" \
  --requests 2000 --conc 50 --warmup 100
```

## Selftest (без ноды)

Если нода не собрана/не запущена, можно проверить сами стресс-скрипты в режиме `mock` (без сети/сокетов):

```bash
./scripts/rpc_stress_suite.sh --user "$RPC_USER" --pass "$RPC_PASS" --selftest
```

Пример для read-only запроса к блокам:

```bash
./scripts/rpc_stress.sh --url "$RPC_URL" \
  --method getblockhash --params '[100]' \
  --requests 2000 --concurrency 25
```

## Примечания

- Лимит запросов RPC по умолчанию **выключен** (`rpc-rate-limit=0` в `geocoin.conf`, либо `--rpc-rate-limit 0`). Если включить ненулевое значение, клиент получит `429` при превышении бюджета за 60 с.
- Для нагруженных методов (`sendrawtransaction`, wallet-операции) нагрузку гоняйте отдельно и с лимитами, чтобы не «забить» mempool/диск.

## Дальнейшие стресс-сценарии (опасные)

Если нужны стресс-тесты, которые меняют состояние, рекомендуемый подход:

- отдельный `data-dir` только для теста (легко удалить/пересоздать)
- запуск на `degree-test`
- явный флаг `ALLOW_DANGEROUS=1` в скрипте (fail-closed по умолчанию)
- лимиты по времени/числу запросов/размеру mempool

Кандидаты для отдельных сценариев:

- mempool admission (batch `sendrawtransaction` с валидными tx)
- wallet send loop (`sendtoaddress`) с контролем UTXO
- witness pruning (`prunenow`) и проверка восстановления (`unprune`) на pruned-ноде
