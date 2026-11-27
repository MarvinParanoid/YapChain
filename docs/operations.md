# Эксплуатация ноды YapChain в Docker (M2.7)

Руководство оператора: сборка образов, инициализация, запуск, наблюдение, резервное копирование и
восстановление валидатора и full node. Архитектура и решения — [ADR-0031](adr/0031-containers.md).
Протокол, консенсус и формат хранилища не зависят от способа запуска.

> Ограничения M2.7. Сеть — тестовая (testnet-параметры genesis). Образы не публикуются в реестр: их собирает
> сам оператор из исходников. Состав валидаторов статический (без governance). Нет light client: ответы RPC
> чужой ноды не проверяются криптографически (docs/cli-network.md). Произвольный откат версии **не**
> поддерживается (см. «Обновление и откат»).

## 1. Что нужно на хосте
- Linux x86_64 или aarch64 (aarch64 проверен только на CI-раннере GitHub, см. §15), Docker Engine ≥ 24 с Compose v2 (`docker compose`) и BuildKit;
- Python ≥ 3.10 (только стандартная библиотека) для `scripts/ops/yapnode.py`;
- исходники YapChain (git), сеть для сборки (архивы зависимостей скачиваются и проверяются по SHA-256).

Все команды ниже выполняются из корня репозитория. `Y="python3 -I scripts/ops/yapnode.py"`.

## 2. Сборка образов
```sh
$Y build                       # yapchain:local и yapchain-cometbft:local, отчёт build/images.json
```
| Образ | Содержимое | Точка входа |
|---|---|---|
| `yapchain:local` | `yapd`, `yap`, `yap-abci-probe` (C++23, Release, GCC 14) | `yapd --home /var/lib/yapd --socket /run/yap/abci/yapd.sock` |
| `yapchain-cometbft:local` | CometBFT v0.40.0 (из архива тега, SHA-256 + `go mod verify`), `yap-abci-probe` | `cometbft-entrypoint start` |

Базы `ubuntu:24.04` и `golang:1.27.4-bookworm` закреплены по дайджесту, пакеты Ubuntu — из снимка архива
(`UBUNTU_SNAPSHOT`), исходники зависимостей — `tools/deps/fetch_deps.py` (SHA-256, повторы, атомарная
публикация). Внутри образов: пользователь `10001:10001`, без компилятора, без ключей. Хеши бинарников —
`/usr/local/share/yapchain/SHA256SUMS`, список пакетов — `runtime-packages.txt`, происхождение CometBFT —
`cometbft.build-info`.

## 3. Раскладка каталога ноды
```
<node>/                 0700, владелец — uid контейнеров ноды
  node.json             роль, chain id, moniker, node id, адрес валидатора (только публичные данные)
  compose.env           параметры для deploy/compose/node.yml (пишет configure)
  cometbft/config/      priv_validator_key.json (0600), node_key.json (0600), genesis.json,
                        genesis.json.sha256, config.toml — в контейнер CometBFT ТОЛЬКО ДЛЯ ЧТЕНИЯ
  cometbft/data/        блоки, состояние консенсуса, priv_validator_state.json (состояние подписи), addrbook
  yapd/data/            LMDB приложения (data.mdb, lock.mdb, yap.lock — блокировка записи)
  abci/                 Unix socket yapd.sock (0600) — общий только для двух контейнеров этой ноды
```
`yapd` в контейнере не имеет сети вообще; CometBFT видит его только через сокет. Ни один каталог не делится
между нодами. Отсутствующий каталог — ошибка `docker compose up` (Docker не создаёт его тихо).

## 4. Порты и межсетевой экран
| Порт | Кто | Публикация по умолчанию | Доступ |
|---|---|---|---|
| 26656/tcp P2P | CometBFT | `0.0.0.0` (node.yml), не публикуется в локальных сетях | открыть для других валидаторов/нод |
| 26657/tcp RPC | CometBFT | `127.0.0.1` | только локально; наружу — через свой обратный прокси с TLS и ограничениями |
| 26660/tcp метрики | CometBFT | не публикуется; `metrics.yml` → `127.0.0.1` | только мониторинг |
| ABCI | yapd | Unix socket, не публикуется никогда | — |

Межсетевой экран хоста валидатора: входящие — только 26656 (лучше — только с адресов других валидаторов и
своих sentry/full node) и SSH администратора. RPC валидатора не публикуется наружу. Docker публикует порты в
обход правил `ufw`/`firewalld` в цепочке `INPUT`: ограничивайте адрес публикации (`P2P_BIND`, `RPC_BIND`) и/или
правила `DOCKER-USER`.

## 5. Локальная разработка: один валидатор (A)
```sh
$Y localnet --dir ~/yapnet1 --validators 1 --project yapdev --subnet-prefix 172.31.240
docker compose --project-directory deploy/compose --env-file ~/yapnet1/compose.env -f deploy/compose/devnet.yml up -d --wait
$Y status --dir ~/yapnet1
```
Это **не** BFT-сеть (нет ни одного допустимого отказа). Тестовый кошелёк (alice, bob, carol, dave, reward-*) —
`~/yapnet1/wallet`, пароль — `~/yapnet1/wallet.password` (0600): только для разработки.

## 6. Четыре валидатора на одном хосте (B) и full node (C)
```sh
$Y localnet --dir ~/yapnet --validators 4 --full-nodes 1
C="docker compose --project-directory deploy/compose --env-file $HOME/yapnet/compose.env -f deploy/compose/localnet.yml"
$C up -d --wait                                              # 4 валидатора (кворум 3 из 4)
$C -f deploy/compose/fullnode.yml up -d --wait fullnode0-yapd fullnode0   # full node с пустыми базами
$Y status --dir ~/yapnet
```
Каждая нода — свои ключи, своя LMDB, свои данные CometBFT, свой сокет; общий только genesis (побайтно) и сеть
`yapnet` с фиксированными адресами `<prefix>.10–.13` (валидаторы) и `.20` (full node). Пиры заданы **адресами**:
CometBFT разрешает имена `persistent_peers` один раз при старте и навсегда отбрасывает имя, которое в этот момент
не разрешается (контейнер пира ещё не поднят) — проверено в M2.7 (full node оставался изолированным после
`down/up`).

## 7. Валидаторы на отдельных Linux-хостах (D)
Церемония genesis — без передачи приватных ключей:
1. **Каждый валидатор** (на своём хосте, под отдельным системным пользователем, например `yapchain`):
   ```sh
   $Y init --dir /srv/yapchain/node --role validator --moniker val-1 --chain-id yapchain-testnet-1
   $Y export --dir /srv/yapchain/node --reward-address tyap1...  --p2p-address 203.0.113.11:26656 > val-1.json
   ```
   Адрес наград — счёт, ключ которого **не** хранится на хосте валидатора (`yap keys create` на рабочей машине).
   `val-1.json` содержит только публичные данные; передайте его координатору.
2. **Координатор** собирает genesis и публикует файл и его SHA-256 по независимому каналу:
   ```sh
   $Y genesis --chain-id yapchain-testnet-1 --validator val-1.json val-2.json val-3.json val-4.json \
       --account tyap1...=1000000YAP --out genesis.json          # печатает sha256 и genesis_hash
   ```
3. **Каждый валидатор** устанавливает genesis (с проверкой SHA-256) и пиров — по IP или по стабильным DNS-именам:
   ```sh
   $Y configure --dir /srv/yapchain/node --genesis genesis.json --genesis-sha256 <sha256> \
       --external-address 203.0.113.11:26656 \
       --peer <node id val-2>@203.0.113.12:26656 --peer <node id val-3>@203.0.113.13:26656 --peer ...
   docker compose --project-directory deploy/compose --env-file /srv/yapchain/node/compose.env \
       -f deploy/compose/node.yml up -d --wait
   ```
   `configure` отказывает, если: другой chain id; не тот SHA-256; ключ валидатора не в наборе (или ключ full node
   в наборе); уже установлен другой genesis; база заполнена, а genesis не установлен. Повторный запуск с теми же
   данными ничего не ломает; изменения `config.toml` сохраняют предыдущую версию `config.toml.prev`.
4. Все узлы стартуют; блоки появляются, когда в сети ≥ 3 из 4 валидаторов (> 2/3 power).

## 8. Full node
```sh
$Y init --dir /srv/yapchain/full --role full --moniker full-1 --chain-id yapchain-testnet-1
$Y configure --dir /srv/yapchain/full --genesis genesis.json --genesis-sha256 <sha256> --peer <id>@<ip>:26656 ...
docker compose --project-directory deploy/compose --env-file /srv/yapchain/full/compose.env -f deploy/compose/node.yml up -d --wait
```
Синхронизируется от genesis через blocksync (state sync выключен), обслуживает RPC, ретранслирует транзакции,
не подписывает. Пустые базы — нормальное начальное состояние; ничего копировать с других нод не нужно.

## 9. Повседневные команды
`N="docker compose --project-directory deploy/compose --env-file <node>/compose.env -f deploy/compose/node.yml"`

| Действие | Команда |
|---|---|
| запуск / остановка | `$N up -d --wait` / `$N stop` (SIGTERM, сначала CometBFT, затем yapd) |
| перезапуск приложения / CometBFT / ноды | `$N restart yapd` / `$N restart cometbft` / `$N restart` |
| логи | `$N logs -f --tail 200 cometbft` ; `$N logs yapd` (строки `Commit: height H, app_hash …`) |
| состояние | `$Y status --dir <node>` (`--json`) |
| баланс | `docker run --rm --network host --entrypoint yap yapchain:local network --rpc http://127.0.0.1:26657 --chain-id <id> query balance tyap1...` |
| подписать офлайн | `docker run --rm --network none -v <wallet>:/w -v <pw>:/pw:ro --entrypoint yap yapchain:local --home /w --password-file /pw tx transfer ... --out /w/tx.hex` |
| отправить подписанную tx | `docker run --rm --network host -v <wallet>:/w:ro --entrypoint yap yapchain:local network --rpc http://127.0.0.1:26657 --chain-id <id> send /w/tx.hex --wait` |
| проверка базы (нода остановлена) | `docker run --rm --network none -v <node>/yapd:/var/lib/yapd:ro --entrypoint yap yapchain:local --home /var/lib/yapd verify-state` |

`yapnode.py status` различает: живость контейнеров (`health`), готовность RPC, `catching_up`, прогресс консенсуса
(`advancing` / `stalled` — возраст последнего блока), число и имена пиров, членство в наборе валидаторов и
voting power, `app_hash` и state root приложения, число перезапусков контейнеров.

**Healthcheck контейнера — это живость, не прогресс.** Нода без кворума или догоняющая нода — `healthy`;
перезапускать её бесполезно. Признак потери кворума: у всех нод `progress: stalled`, контейнеры здоровы, пиров
меньше 3. Признак проблем с синхронизацией: `catching_up: true` долгое время, 0 пиров.

## 10. Безопасность
- Ключи: `priv_validator_key.json`, `node_key.json` — `0600` в каталоге `0700`, владелец — uid ноды; в образах и
  контексте сборки их нет (белый список `.dockerignore`; проверка слоёв образа в приёмке M2.7). Пароли кошелька
  передаются только файлом (`--password-file`), никогда аргументом.
- Контейнеры: не root, `read_only` корень, `cap_drop: ALL`, `no-new-privileges`, лимиты памяти/CPU/pids
  (`YAPD_MEM_LIMIT`, `COMETBFT_MEM_LIMIT`, `*_CPUS`), без privileged, без Docker socket, `yapd` без сети.
- RPC только на `127.0.0.1`; `unsafe` RPC выключен; CORS пуст. Административного интерфейса нет.
- Членство в группе `docker` эквивалентно root на хосте: выдавайте его только администратору ноды.
- **Не гарантируется**: защита от root на хосте, от компрометации ядра или Docker; аппаратное хранение ключа
  (HSM/KMS, remote signer) — не реализовано; защита RPC от DoS без внешнего прокси.

## 11. Резервное копирование
Только **офлайн** — копия «живой» LMDB или данных CometBFT не является резервной копией.
```sh
$N stop                                                  # сначала остановить ноду
$Y backup --dir <node> --out /backup/node-$(date +%F).tar          # без приватных ключей
$Y backup --dir <node> --out /secure/node-keys.tar --include-keys  # с ключами: хранить отдельно, шифровать
$Y verify-backup /backup/node-....tar
$N up -d --wait
```
`backup` отказывает, если контейнеры ноды работают; держит блокировку записи `yapd` (`yap.lock`) всё время
копирования; проверяет базу `verify-state` и записывает высоту и state root в манифест; архив `0600`, атомарно,
с SHA-256 каждого файла. Ключи валидатора храните отдельно от данных (оффлайн, зашифрованными).

## 12. Восстановление
**Full node** (или нода, которую не надо возвращать валидатором) — два пути:
- с нуля: `init --role full` + `configure` с тем же genesis → blocksync от genesis (самый простой и проверенный путь);
- из резервной копии: `$Y restore --archive A.tar --dir <new> --as full` → новый ключ валидатора (не в наборе),
  база проверена `verify-state` и совпадает с манифестом; затем `configure` и запуск. Нода догоняет сеть.

**Валидатор — аварийное восстановление (ручной runbook).** Главная опасность — двойная подпись: два процесса с
одним ключом или устаревшее `priv_validator_state.json`. Восстановление ключа без состояния подписи небезопасно.
1. **Выведите старый валидатор из эксплуатации окончательно**: остановите контейнеры, отключите хост от сети
   или уничтожьте его копию ключа. Если нет уверенности, что старый экземпляр не запустится, — **не продолжайте**.
2. Узнайте текущую высоту сети по ≥ 2 независимым нодам (`status`), возьмите `F = текущая высота + запас`
   (например, +10).
3. Восстановите в **новый** каталог:
   ```sh
   $Y restore --archive node-keys.tar --dir /srv/yapchain/node --as validator \
       --validator-key /secure/priv_validator_key.json --signing-floor-height F --original-decommissioned
   ```
   Состояние подписи поднимается до «пола» `F` (только вверх, никогда вниз): CometBFT откажется подписывать
   что-либо на высотах ≤ F и начнёт подписывать с F+1. Отдельно: `$Y signing-floor --dir <node> --height F`.
4. `configure` (тот же genesis и SHA-256, пиры), запуск, `status`: нода догоняет сеть и начинает подписывать
   выше F. Проверьте, что в блоках нет evidence двойной подписи.

Проверено в M2.7 (`test_container_suite.py`, сценарий 15b): валидатор восстановлен из копии с ключами после
вывода оригинала, первая подпись — выше пола, evidence нет; восстановление full node в изолированной сети —
та же высота и state root, что у сети.

## 13. Диагностика
| Симптом | Где смотреть | Причина и действие |
|---|---|---|
| `cometbft` перезапускается, в логе `refusing to start ... genesis.json has SHA-256` | `$N logs cometbft` | genesis подменён; верните исходный (genesis не заменяется) |
| `panic: state.AppHash does not match AppHash after replay` | `$N logs cometbft` | база `yapd` не от этой цепочки/ноды; ничего не удалять, вернуть правильную базу |
| `yapd: cannot start: ...` (код 3) | `$N logs yapd` | права/место/занятая блокировка; исправить — контейнер поднимется сам |
| `yapd: fail-stop` (код 2) | `$N logs yapd` | несогласованность или отказ хранилища; база цела на последнем Commit; см. ADR-0028 |
| у всех нод `stalled`, контейнеры `healthy` | `status` | потеря кворума: верните валидаторы (нужно > 2/3 power) |
| `catching_up: true`, 0 пиров | `status`, лог `Error in peer's address` | пир не разрешился при старте: используйте IP/стабильный DNS, перезапустите CometBFT |
| `dependency failed to start: ... unhealthy` при `up` | `$N ps`, `$N logs yapd` | yapd не отвечает на сокете (права каталогов, uid) |

## 14. Обновление и откат
- Обновление образов: соберите новые образы, `$N stop`, **резервная копия**, `$N up -d --wait` с новыми тегами
  (`YAPCHAIN_IMAGE`, `COMETBFT_IMAGE` в `compose.env`), `status`. Совместимость форматов между версиями
  проверяется отдельно для каждого релиза.
- **Откат на более старую версию не поддерживается в общем случае**: более старый `yapd` может не открыть базу,
  записанную новым (версия схемы ADR-0022), а CometBFT не поддерживает откат своих данных. Откат = восстановление
  из резервной копии, сделанной старой версией, и догон сети — не запуск старого бинарника на новых данных.
- Автоматических обновлений нет.

## 15. Ресурсы (измерено в M2.7) и предварительные требования к VPS
Стенд: один хост, AMD Ryzen 7 9700X (8 ядер / 16 потоков), 32 GB RAM, NVMe (btrfs), Linux 7.3.5, Docker 29.10
(overlayfs, cgroup v2), образы x86_64. Все 5 нод (4 валидатора + full node) **на одном хосте**, P2P по мосту Docker,
`timeout_commit = 300ms`. Нагрузка — контейнерный soak (`tests/docker/test_container_soak.py`, seed 20261127):
16 отправителей, конвейер глубиной 3 на отправителя, 10 % Burn, 9 отказов. Это **не** измерение предельной
пропускной способности: насыщение не проверялось, нагрузку ограничивал генератор.

| Метрика | Значение |
|---|---|
| Размер образа (несжатый) | `yapchain` 139,2 MB; `yapchain-cometbft` 209,2 MB |
| Запуск до `healthy` (обе ноды контейнера, `up --wait`) | 11,3–11,9 с (1 и 4 валидатора; включает интервал healthcheck 10 с) |
| Память, простой (медиана / максимум) | `yapd` 1,7 / 2,1 MiB; CometBFT 39 / 48 MiB |
| Память под нагрузкой (медиана / максимум) | `yapd` 2,5 / 4,9 MiB; CometBFT 65 / 83 MiB |
| CPU под нагрузкой (медиана / p95, % одного ядра) | `yapd` 0,7 / 2,5; CometBFT 2,0 / 4,1 |
| Интервал блоков под нагрузкой | медиана 0,474 с, p95 0,552 с, максимум 28,2 с (потеря кворума) |
| Исполнено транзакций | 15 959 за 534 с нагрузки (≈ 30 tx/с; не предел) |
| Диск: рост на ноду | 0,4 → 29–35 MB за 1025 блоков с 15 959 транзакциями (≈ 30 KB на блок под этой нагрузкой) |
| Догон валидатора после простоя | 124 блока за 4,2 с (blocksync) |
| Синхронизация full node от genesis | 148 блоков за 11,4 с от `compose up` (включая запуск) |

**Повтор в CI** (run 46015785139, коммит `2a1cd1c`; раннер GitHub `ubuntu-24.04`, x86_64, общий виртуальный
хост — абсолютные значения с локальным стендом напрямую не сравнимы; тот же seed, `timeout_commit = 300ms`):

| Метрика | Значение |
|---|---|
| Блоков / длительность soak | 1040 (строгая проверка 1–1008 на всех нодах) / 675,6 с; задание целиком 32 мин 44 с (сборка образов 1206 с) |
| Отказы | 10: все 7 видов (crash yapd, crash CometBFT, остановка валидатора ×2, потеря кворума ×2, crash двух CometBFT, разделение ×2, перезапуск full node) |
| Память, простой (медиана / максимум) | `yapd` 1,8 / 2,0 MiB; CometBFT 21,7 / 32,8 MiB |
| Память под нагрузкой (медиана / максимум) | `yapd` 2,3 / 6,5 MiB; CometBFT 50,3 / 66,2 MiB |
| CPU под нагрузкой (медиана / p95, % одного ядра) | `yapd` 1,1 / 3,5; CometBFT 4,4 / 7,0 |
| Интервал блоков под нагрузкой | медиана 0,504 с, p95 0,831 с, максимум 25,6 с (потеря кворума) |
| Исполнено транзакций | 13 992 за 605,8 с нагрузки (≈ 23 tx/с; не предел) |
| Диск: рост на ноду | 0,08–0,38 → 28,5–32,1 MB за 1040 блоков |
| Синхронизация full node | 100 блоков за 11,4 с от `compose up` |
| Итог | `verify-state` всех пяти баз: высота 1040, один корень `2f497961…` |

Приёмочный набор на нативных раннерах того же запуска: x86_64 — 519,2 с; aarch64 (`ubuntu-24.04-arm`) — 535,1 с;
сборка образа `yapchain` 1011,6 / 1490,8 с, `yapchain-cometbft` 50,1 / 54,0 с; размер по отчёту сборки
`yapchain` 94,0 / 115,9 MB, `yapchain-cometbft` 140,0 / 158,9 MB (x86_64 / aarch64).

Память по `docker stats` не включает страничный кеш: страницы LMDB (`mmap` файла) и данные goleveldb CometBFT
живут в кеше ОС и растут с размером базы.

**Предварительно, для testnet** (по этим измерениям с запасом на рост базы, пики и наблюдение; уточняется
по мере роста сети):
| Роль | vCPU | RAM | Диск | Сеть |
|---|---|---|---|---|
| Валидатор | 2 | 4 GB | 40–80 GB SSD/NVMe (рост ≈ 30 KB на блок под нагрузкой; следить за ростом) | статический IP, открыт 26656/tcp |
| Full node / RPC | 2 | 4 GB | то же | RPC только через прокси с TLS |

Не измерялось: разные хосты и реальные задержки WAN, длительная работа (дни), рост базы при годовой истории,
нагрузка до насыщения. aarch64: приёмочный набор целиком проходит на нативном CI-раннере GitHub
(`ubuntu-24.04-arm`); soak и ресурсы на aarch64 не измерялись, внешние aarch64-хосты (VPS, собственное
оборудование) не проверялись.
