# YapChain

> **Экономика настоящая. Ценность сомнительная.**

YapChain — open-source блокчейн мемной экосистемы с собственной валютой **YAP**.
Назначение абсурдно, исполнение — нет: настоящие подписи Ed25519, независимые ноды,
проверяемая история, BFT-консенсус и детерминированная state machine на C++23.

## Что это

- **Account-based ledger**: балансы и nonce у адресов, без UTXO и без VM.
- **Нативная валюта YAP**: genesis-распределение и эмиссия за блоки; минимальная единица — `yip` (1 YAP = 1 000 000 yip).
- **Permissioned BFT**: 4 известных валидатора, выдерживает 1 византийского.
- **Встроенные операции** вместо смарт-контрактов: `Transfer`, `Burn`, `ClaimAchievement`.
- **Мемные достижения**: внешние приложения (Мемпаспорт, Мемонатор, Yapped) выдают подписанные аттестации,
  пользователь сам подаёт claim и получает YAP из пула наград. Без YapChain эти приложения работают как прежде.
- **Адреса** вида `tyap1…` (Bech32m).

## Статус

- **M0** — архитектура и протокол (ревизия r2) — завершён.
- **M1.1 Foundation** — реализован: сборка, зафиксированные зависимости, базовые типы и checked-арифметика,
  YCS, SHA-256 и tagged hash, Ed25519, Bech32m и адреса, независимые test vectors.
- **M1.2 Ledger** — реализован: транзакции `Transfer`/`Burn` (формат `ClaimAchievement` декодируется, исполнение — M3),
  валидация в нормативном порядке, детерминированная state machine в памяти, комиссии и эмиссия, genesis,
  Sparse Merkle state root, execution engine с жизненным циклом ABCI++; всё сверено с независимой Python-реализацией.
- **M1.3 Persistence** — реализован: LMDB-хранилище, атомарный commit высоты, персистентный инкрементальный SMT,
  проверка и восстановление состояния, replay, crash-тесты с SIGKILL.
- **M1.4 Dev-chain и CLI** — реализован: зашифрованный keystore, транзакции, подписанные dev-блоки,
  локальная dev-цепочка, запросы и проверка состояния через `yap`.
- **M2.0–M2.3 CometBFT** — реализованы: CometBFT v0.40.0 из проверенных исходников, ABCI++ транспорт
  (protobuf, Unix socket), адаптер над детерминированным ядром, демон `yapd`, локальная сеть из **одного**
  валидатора с настоящим CometBFT и живой E2E-тест с восстановлением после сбоев.
- **M2.4 Четыре валидатора** — реализован: локальная сеть из четырёх независимых валидаторов (CometBFT + `yapd`
  у каждого), живые тесты детерминизма, распределённых транзакций и RECHECK, наград по реальным голосам и догона
  отставшей ноды через blocksync.
- **M2.5 Отказоустойчивость** — проверена: потеря и восстановление кворума, разделения сети 3+1 и 2+2, задержки и
  нестабильная связь, аварии нод, full node без права голоса, длительный soak с отказами
  ([docs/fault-tolerance.md](docs/fault-tolerance.md)).
- **M2.6 Сетевой CLI** — реализован: `yap network` — запросы состояния, блоков и транзакций, отправка транзакций,
  подписанных локальным keystore (или офлайн), и ожидание финализации через RPC CometBFT с проверкой chain id
  ([docs/cli-network.md](docs/cli-network.md)).
- **M2.6.2 C++23** — принят: весь собственный код — C++23 (GCC ≥ 13, Clang ≥ 18), протокол не изменился.
- **M2.7 Docker** — принят: воспроизводимые образы `yapd`/`yap` и CometBFT, Compose для одиночной ноды,
  четырёх валидаторов и full node, инициализация, состояние, резервное копирование и восстановление
  ([docs/operations.md](docs/operations.md)). Образы и приёмочный набор проверены нативно на x86_64 и aarch64
  в CI (раннеры GitHub); развёртывания на внешних aarch64-хостах не проверялись.
- M2.8+ — впереди, см. [docs/roadmap.md](docs/roadmap.md).

Локальная сеть из четырёх валидаторов ([scripts/localnet](scripts/localnet/README.md)) выдерживает отказ одного
валидатора; публичного testnet нет. Dev-цепочка — локальный инструмент разработки с одним производителем блоков.

## Быстрый старт (локальная dev-цепочка)

```sh
yap keys create alice                      # пароль спрашивается без эха (или --password-file)
yap keys create bob
yap genesis new --account <alice>=100YAP \
    --validator <pubkey-hex>:10:<bob>:validator-1 --out genesis.json
yap dev init --genesis genesis.json        # создаёт ключ dev-производителя
yap tx transfer --from alice --to <bob> --amount 1.5YAP --out t1.hex   # nonce/срок из локального state
yap dev produce --tx t1.hex --require-all
yap query account <alice>
yap query chain
yap verify-state
yap dev export --out journal.hex           # журнал для replay на другой ноде
```

Коды выхода: 0 — успех, 1 — командная строка, 2 — отклонено/невалидно, 3 — хранилище, 4 — пароль/keystore,
5 — внутренняя ошибка; у сетевых команд ещё 6 — сеть/RPC, 7 — не найдено, 8 — исход неизвестен, 9 — исполнена с
ошибкой, 10 — другая цепочка. Существующие выходные файлы не перезаписываются без `--force`.

## Работа с сетью (`yap network`)

```sh
yap keys create alice
yap network config set --rpc https://rpc.example.org:443 --chain-id yapchain-testnet-1   # доверенный RPC
yap network status                                   # chain id проверен, высота, catching_up
yap network query balance <tyap1...>
yap network transfer --from alice --to <tyap1...> --amount 1.5YAP --wait               # подтвердить [y/N]
yap network tx <HASH>                                # результат по хешу; повторной отправки нет
```
Подпись защищает только авторизацию: ответы RPC (балансы, высоты, «финализация») не проверяются криптографически —
выбирайте доверенный узел. Таймаут ожидания (код 8) не означает отказ — проверьте позже по хешу. Подробно:
[docs/cli-network.md](docs/cli-network.md).

## Сборка и тесты

Требования: Linux x86_64 или aarch64, CMake ≥ 3.25, Ninja, make, компилятор C++23 — GCC ≥ 13 или Clang ≥ 18 со
стандартной библиотекой libstdc++ (libc++ не поддерживается), Python 3 (для векторов). Проверенные пары компилятор +
libstdc++ и доступные возможности C++23 — в [ADR-0030](docs/adr/0030-cxx23.md); в CI — GCC 14 и Clang 18.
Зависимости (libsodium 1.0.22, Catch2 3.16.1, nlohmann/json 3.12.0, LMDB 0.9.36, CLI11 2.7.2, а для ABCI —
abseil 20260817.0 и protobuf v36.2) скачиваются CMake и проверяются по SHA-256 (офлайн: `-DYAP_DEPS_DIR=<каталог>`).
protobuf и abseil собираются при первой конфигурации каждого каталога сборки (около 1,5 мин); без ABCI —
`-DYAP_WITH_ABCI=OFF`. Сетевым командам CLI нужен системный libcurl ≥ 7.85 с заголовками (`libcurl4-openssl-dev`,
[ADR-0029](docs/adr/0029-rpc-client.md)); без них — `-DYAP_WITH_NETWORK=OFF`. Для `yapd` с CometBFT нужен Go ≥ 1.25 (см. [scripts/localnet](scripts/localnet/README.md)).

```sh
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan     # ASan + UBSan
cmake --preset release && cmake --build --preset release && ctest --preset release
CC=clang CXX=clang++ cmake --preset tsan -B build/clang-tsan && cmake --build build/clang-tsan   # ThreadSanitizer
python3 -I tools/refgen/refgen.py --out test-vectors    # перегенерировать эталонные векторы
```

## Документация

| Документ | О чём |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Модули, зависимости, CMake, библиотеки |
| [docs/protocol.md](docs/protocol.md) | Сериализация, адреса, транзакции, state, Merkle, исполнение блоков |
| [docs/consensus.md](docs/consensus.md) | Выбор BFT-движка, интеграция, восстановление после сбоев |
| [docs/economics.md](docs/economics.md) | Токеномика закрытого testnet |
| [docs/attestations.md](docs/attestations.md) | Подписанные аттестации и начисление наград |
| [docs/threat-model.md](docs/threat-model.md) | Модель угроз |
| [docs/cli-network.md](docs/cli-network.md) | Сетевой режим CLI: команды, настройки, доверие к RPC, коды выхода |
| [docs/fault-tolerance.md](docs/fault-tolerance.md) | Результаты проверок отказоустойчивости сети (M2.5) |
| [docs/operations.md](docs/operations.md) | Эксплуатация ноды в Docker: сборка, развёртывание, состояние, резервное копирование и восстановление |
| [docs/roadmap.md](docs/roadmap.md) | Этапы, scope и тест-план M1 |
| [docs/adr/](docs/adr/) | Архитектурные решения (ADR) |

В документах статусы решений помечены так:
**DECIDED** — утверждено, **PROPOSED** — предлагается, **OPEN** — требует решения.

## Архитектура в одной схеме

```mermaid
flowchart LR
    U[Пользователь / CLI] -->|signed tx| N
    A[Мемпаспорт / Мемонатор / Yapped] -.->|signed attestation| U
    subgraph N[Нода YapChain]
        C[CometBFT<br/>консенсус + P2P] <-->|ABCI++| APP[yapd: C++23<br/>детерминированная state machine]
        APP --> DB[(LMDB: state + SMT)]
    end
    N <-->|P2P| N2[Другие ноды и валидаторы]
```

## Лицензия

[Apache-2.0](LICENSE).
