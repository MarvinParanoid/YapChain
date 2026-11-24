# Localnet: настоящий CometBFT v0.40.0, один или четыре валидатора

Воспроизводимая локальная сеть для проверки ABCI++:
- **один валидатор** (M2.3, по умолчанию) — один процесс CometBFT, один `yapd`. **Это не BFT-сеть**: она не
  выдерживает ни одного отказа и нужна для прогона реального протокола от начала до конца;
- **четыре независимых валидатора** (M2.4, `--validators 4`) — см. раздел ниже. Выдерживает отказ одного
  валидатора (f = 1 при n = 4).

Всё (ключ валидатора, ключ ноды, кошелёк, пароль, базы) создаётся в указанном каталоге с правами `0700`.
Каталог не должен лежать в репозитории; сгенерированные ключи и базы не коммитятся.

## Требования
- собранные `yap` и `yapd` (`cmake --preset debug && cmake --build --preset debug`);
- Go ≥ 1.25 для сборки CometBFT;
- Python 3 (только стандартная библиотека).

## Шаги
```sh
# 1. CometBFT из проверенного архива тега (SHA-256 и коммит — ADR-0027); go.sum проверяется Go.
python3 -I scripts/localnet/localnet.py build-cometbft --download --out build/cometbft
#    офлайн: --deps-dir <каталог с cometbft-v0.40.0.tar.gz>

# 2. Временное окружение: cometbft init, ключи кошелька через `yap keys`, genesis через `yap genesis new`,
#    genesis.json CometBFT с нашим genesis в app_state, config.toml (Unix socket, только 127.0.0.1, без PEX).
python3 -I scripts/localnet/localnet.py init --dir /tmp/yapnet --yap build/debug/src/cli/yap \
    --cometbft build/cometbft/cometbft

# 3. Запуск yapd, затем CometBFT; ожидание готовности (высота ≥ 2).
python3 -I scripts/localnet/localnet.py start --dir /tmp/yapnet --yapd build/debug/src/node/yapd \
    --cometbft build/cometbft/cometbft

python3 -I scripts/localnet/localnet.py status --dir /tmp/yapnet
# 4. Корректная остановка: сначала CometBFT, затем yapd (SIGTERM).
python3 -I scripts/localnet/localnet.py stop --dir /tmp/yapnet
```

Адреса RPC, сокета и кошелька — в `<dir>/localnet.json`. Запросы состояния — через RPC CometBFT:
`curl '<rpc>/abci_query?path="/account/<addr>"'` (также `/chain`, `/params`; значение — JSON в base64).
Транзакцию собирает и подписывает CLI (`yap --home <dir>/wallet --password-file <dir>/wallet.password tx transfer …
--nonce N --expires H --chain-id yapchain-localnet-1`), отправка — `<rpc>/broadcast_tx_sync?tx=0x<hex>`.

## Что настраивается
| Где | Значение | Почему |
|---|---|---|
| genesis.json | `app_state` = genesis YapChain, `app_hash` = `""`, `initial_height` = `"1"` | protocol.md §11.1, §11.4 |
| genesis.json | `consensus_params.version.app` = `"1"`, `abci.vote_extensions_enable_height` = `"0"` | ADR-0028 п. 5–6 |
| config.toml | `proxy_app = "unix://<dir>/yapd/yapd.sock"`, `abci = "socket"` | сокет `0600`, не доступен извне |
| config.toml | RPC и P2P только на `127.0.0.1`, `pex = false` | локальная сеть |
| config.toml | `mempool.type = "flood"`, `recheck = true`, `timeout_commit = "1s"` | чек-лист M2 |

## yapd
`yapd --home <dir> [--socket <path>] [--trace-abci]`: база — `<home>/data` (LMDB, эксклюзивная блокировка записи;
база dev-цепочки отклоняется). Коды выхода: 0 — остановлен сигналом, 1 — командная строка, 2 — fail-stop во время
работы, 3 — не удалось запуститься. Обычный `yapd` не содержит тестовых хуков. Для тестов восстановления
собирается отдельный бинарник `yapd-test-hooks` (только вместе с тестами, `YAP_BUILD_TESTS`): с переменной
`YAPD_TEST_CRASH=before_commit:H|after_commit:H` он убивает себя SIGKILL до или после durable-записи высоты `H`
и при старте пишет в лог предупреждение «TEST BUILD». В рабочей среде его не используют.

## Четыре валидатора (M2.4)
```sh
python3 -I scripts/localnet/localnet.py init --dir build/disk-tmp/n4 --validators 4 \
    --yap build/debug/src/cli/yap --cometbft build/cometbft/cometbft
python3 -I scripts/localnet/localnet.py start  --dir build/disk-tmp/n4 --yapd build/debug/src/node/yapd \
    --cometbft build/cometbft/cometbft            # все ноды; ждёт высоту ≥ 2 на каждой
python3 -I scripts/localnet/localnet.py status --dir build/disk-tmp/n4   # высота, пиры, app_hash каждой ноды

# Отдельная нода (индекс 0..3): остановка и запуск с существующими данными, без пересоздания.
python3 -I scripts/localnet/localnet.py stop  --dir build/disk-tmp/n4 --node 2
python3 -I scripts/localnet/localnet.py start --dir build/disk-tmp/n4 --node 2 --yapd build/debug/src/node/yapd \
    --cometbft build/cometbft/cometbft
python3 -I scripts/localnet/localnet.py stop  --dir build/disk-tmp/n4    # вся сеть: сначала все CometBFT, затем все yapd
```
Путь сокета ограничен ~100 байтами, поэтому каталог должен быть коротким (`build/disk-tmp/…` или `/tmp/…`).

Раскладка каталога:
```
<dir>/network.json          chain_id, genesis_hash, sha256 genesis.json, адреса, описание нод
<dir>/wallet/, wallet.password   ключи alice, bob, carol, dave и reward-0..3 (пароль 0600)
<dir>/yap-genesis.json      genesis YapChain (4 валидатора, power 10, у каждого свой reward-счёт)
<dir>/node<I>/localnet.json метаданные ноды (те же ключи, что у одновалидаторной сети)
<dir>/node<I>/cometbft/     свой priv_validator_key.json, node_key.json, config.toml, данные CometBFT
<dir>/node<I>/yapd/         своя LMDB (data/) и свой сокет yapd.sock
<dir>/node<I>/{yapd,cometbft}.log, *.pid
```
Каждая нода — отдельные процессы CometBFT и `yapd`, своя LMDB, свой сокет, свои порты RPC/P2P, свой
validator key и node key, свои логи и свой reward-счёт. Общего изменяемого состояния у нод нет: они
обмениваются только через P2P CometBFT.

| Где | Значение | Почему |
|---|---|---|
| genesis.json | один файл, побайтно одинаковый у всех нод (sha256 в `network.json`); все 4 валидатора, power 10 | одна цепочка |
| config.toml | `persistent_peers` = остальные три ноды (`id@127.0.0.1:port`) | P2P без PEX |
| config.toml | `allow_duplicate_ip = true`, `addr_book_strict = false` | все ноды на одном 127.0.0.1 |
| config.toml | `statesync.enable = false`; blocksync включён (по умолчанию) | ADR-0028 п. 4, догон отставшей ноды |
| genesis.json | `vote_extensions_enable_height = 0` | vote extensions выключены |

Отставшая нода при запуске сначала работает в режиме blocksync (запись CometBFT
`Time to switch to consensus mode! height=N` показывает, до какой высоты блоки пришли через blocksync),
затем переходит в consensus. Если нода простояла недолго, она может догнать через обычный gossip
консенсуса — это не blocksync.

## Управляемые сетевые отказы (M2.5)
`init --validators 4 --fault-proxy` направляет каждую P2P-связь i→j через свой порт непривилегированного TCP-прокси
[`netfault.py`](netfault.py) (asyncio, только stdlib, без root). `persistent_peers` ноды i указывают на порты прокси,
PEX выключен — другого маршрута между нодами нет. RPC (TCP) и ABCI (Unix socket) через прокси не идут. Прокси
запускается `start` и останавливается `stop` последним; события пишутся с временем в `<dir>/faults.log`.
```sh
L="python3 -I scripts/localnet/localnet.py"
$L init  --dir build/disk-tmp/f4 --validators 4 --fault-proxy --yap … --cometbft …
$L start --dir build/disk-tmp/f4 --yapd … --cometbft …
$L fault --dir build/disk-tmp/f4 '{"cmd": "partition", "groups": [[0, 1, 2], [3]]}'   # 3+1
$L fault --dir build/disk-tmp/f4 '{"cmd": "heal"}'
$L fault --dir build/disk-tmp/f4 '{"cmd": "delay", "ms": 150, "jitter_ms": 50}'        # ОДНОСТОРОННЯЯ, RTT = 2×
$L fault --dir build/disk-tmp/f4 '{"cmd": "flap", "period_s": 5, "down_s": 1}'         # периодические разрывы
$L fault --dir build/disk-tmp/f4 '{"cmd": "reset", "probability": 0.3}'                # сброс TCP-соединений
$L fault --dir build/disk-tmp/f4 '{"cmd": "status"}'                                   # связи, байты, задержка
$L add-node --dir build/disk-tmp/f4 --cometbft …     # full node без права голоса (не в наборе валидаторов)
$L start --dir build/disk-tmp/f4 --node 4 --yapd … --cometbft …
```
Full node (`add-node`, M2.5): свои ключи (validator key не входит ни в какой набор), тот же `genesis.json` побайтно,
**пустые** базы CometBFT и `yapd`, валидаторы — persistent peers (через прокси, если он есть). Синхронизируется от
genesis через blocksync, обслуживает RPC и Query, ретранслирует транзакции, не голосует и не получает наград.
State sync не используется.
Потеря отдельных пакетов **не моделируется**: для неё нужен tc/netem (root, модуль ядра); сброс TCP-соединения —
это другой отказ. Прокси и тестовые хуки — только тестовые инструменты; в `yapd` их нет.

## Живой E2E-тест
`tests/integration/test_live_cometbft.py <yap> <yapd> <cometbft> <yapd-test-hooks>` — 17 шагов и два сценария
сбоя (сбой — через `yapd-test-hooks`, восстановление — обычным `yapd`); в CTest — `integration:live_cometbft`.

`tests/integration/test_live_multival.py <yap> <yapd> <cometbft>` — четыре валидатора: консенсус и детерминизм,
распределённые транзакции и живой RECHECK, награды по реальным голосам (COMMIT/ABSENT/NIL), остановка валидатора и
его догон через blocksync, verify-state всех четырёх баз; в CTest — `integration:live_multival`.

Оба теста регистрируются, если при конфигурации задан `-DYAP_COMETBFT_BIN=<путь>`.
