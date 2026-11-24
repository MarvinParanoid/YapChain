# Localnet: один валидатор, настоящий CometBFT v0.40.0

Минимальная воспроизводимая локальная сеть для проверки ABCI++ (M2.3): один процесс CometBFT, один `yapd`,
один валидатор. **Это не BFT-сеть**: она не выдерживает ни одного отказа и нужна только для прогона реального
протокола от начала до конца. Сеть из 4 валидаторов — M2.4.

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
работы, 3 — не удалось запуститься. Переменная `YAPD_TEST_CRASH=before_commit:H|after_commit:H` — только для
тестов восстановления: процесс убивает себя SIGKILL до или после durable-записи высоты `H`.

## Живой E2E-тест
`tests/integration/test_live_cometbft.py <yap> <yapd> <cometbft>` — 17 шагов и два сценария сбоя; в CTest
регистрируется как `integration:live_cometbft`, если при конфигурации задан `-DYAP_COMETBFT_BIN=<путь>`.
