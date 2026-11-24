# ADR-0027: Получение CometBFT и protobuf для M2

**Статус:** DECIDED (M2.0) · дополняет [ADR-0026](0026-cometbft-version.md) (версия не меняется: **v0.40.0**) и
[ADR-0018](0018-dependency-pinning.md)

## Проверка выбранной версии (24.11.2026, по тегам и коду, не по документации)
| Пункт | Результат |
|---|---|
| Официальный релиз | `v0.40.0` существует: аннотированный тег `06b4cbf4b776554bd2b95fc39c80991e7529e765` → коммит `0880b4d378f347ab16e54ec677ff50d803f37d62` |
| Патч-релизы | нет (`git ls-remote --tags 'v0.40*'` → только `v0.40.0`); последняя v0.38 — `v0.38.26` |
| Бинарные артефакты | **у релизов нет assets** (ни для x86_64, ни для aarch64) → сборка из исходников |
| Исходники | архив тега `cometbft-v0.40.0.tar.gz`, SHA-256 `c458c51a452318a849905cdeb1eb1c4437cff0b5412bb6ca5d948c7c37d77a8a` |
| Модули Go | `go build -mod=readonly` проверяет все зависимости по `go.sum` и `sum.golang.org`; `go mod verify`: *all modules verified* |
| Поддержка | семейство релизов 2026.1 (ADR-0026) |
| Строка версии | `cometbft version` печатает `0.39.0` — константа `TMCoreSemVer` не обновлена в теге (косметика); идентичность сборки фиксируем коммитом тега через `-ldflags -X …TMGitCommitHash` |

### Security advisories (GitHub, cometbft/cometbft)
| Advisory | Исправлено в | v0.40.0 |
|---|---|---|
| GHSA-c32p-wcqj-j677 CSA-2026-001 «Tachyon» (critical, BFT time) | v0.38.21 | **исправлено**: в коде тега есть проверка `validator address mismatch at index` в `types/validation.go` и `MedianTime(...) (time.Time, error)` в `state/state.go` — ровно изменения v0.38.20→v0.38.21; плюс #5901 (MedianTime учитывает только Commit) |
| GHSA-hrhf-2vcr-ghch ASA-2025-003 (BitArray, high) | v0.38.19 | исправлено (CHANGELOG v0.40.0: «Validate BitArray mismatched Bits and Elems length») |
| GHSA-r3r4-g7hq-pq4f, GHSA-22qq-3xwm-r5x4 (2025-02) | v0.38.17 | v0.40 — потомок более поздней линии v0.38 (main перебазирован на v0.38.x, PR #5326); отдельно не проверялось кодом |
| более ранние (2023–2024) | ≤ v0.38.15 | то же |

## Решение: как получаем CometBFT
1. Архив исходников тега скачивается и проверяется по SHA-256 выше (или берётся из `YAP_DEPS_DIR`).
2. Сборка: `GOFLAGS=-mod=readonly go build -trimpath -ldflags "-X github.com/cometbft/cometbft/version.TMGitCommitHash=0880b4d3" ./cmd/cometbft`
   с Go ≥ 1.25 (минимум из `go.mod`). Все модули проверяются по `go.sum`; непроверенные бинарники не скачиваются.
3. Хеш итогового бинарника зависит от тулчейна Go и поэтому не является точкой доверия; доверие — к хешу исходников и
   `go.sum`. Локально (go1.27.4, x86_64) SHA-256 бинарника записывается скриптом рядом с ним.
4. CI и aarch64: тот же скрипт на нативном раннере с `actions/setup-go` фиксированной версии.

## Решение: protobuf для ABCI
- Используем официальный `protoc` и сгенерированные C++-биндинги (не свой декодер protobuf).
- **protobuf v36.2** (`protobuf-36.2.tar.gz`, SHA-256 `3d9642a662d10e68ebae5e53f14dcce5105684212d5078f8e0d47d1ab3ae6b64`) и
  **abseil-cpp 20260817.0** (SHA-256 `f7e05179df39c45434cad433f5783840bb3788ef322976f9138bc6b72b3a107d`) собираются из
  исходников при конфигурации (Release, без санитайзеров, как libsodium); системные копии не используются.
- Схемы: ровно 5 файлов тега v0.40.0 (`tendermint/abci/types.proto`, `tendermint/types/{params,validator}.proto`,
  `tendermint/crypto/{keys,proof}.proto`) и `gogoproto/gogo.proto` из `github.com/cosmos/gogoproto v1.7.2` (версия из
  `go.mod` CometBFT, содержимое проверено `go.sum`) копируются в `third_party/cometbft-proto/` вместе с файлом
  контрольных сумм; тест сверяет копии с хешами.

## Последствия
- Первая конфигурация каждого каталога сборки дольше на сборку protobuf/abseil.
- Для запуска live-тестов нужен Go; без него live-тесты пропускаются с явным сообщением (не считаются пройденными).
