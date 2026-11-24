# Чек-лист совместимости с CometBFT v0.40.0 (для M2)

Статусы: **OK** — ядро уже соответствует; **M2** — сделать в адаптере ABCI; **CHECK** — проверить на живом узле.
Источники: proto и код на теге v0.40.0 (`proto/tendermint/abci/types.proto`, `abci/types/messages.go`,
`state/execution.go`, `consensus/replay.go`, `abci/client/socket_client.go`), спецификация v0.38.26 `spec/abci/`,
сайт docs.cosmos.network/cometbft/latest. Пункты, подтверждённые только кодом, помечены *(код)*.

## Транспорт
| Пункт | Статус |
|---|---|
| Кадр: protobuf `Request`/`Response`, префикс длины — **unsigned varint** (`binary.PutUvarint`), максимум `MaxInt32` | M2 |
| Номера полей `oneof` в `Request` и `Response` **различаются** (например, finalize_block = 20 / 21) | M2 |
| `Flush`: ответить `ResponseFlush` и сбросить буфер; ответы — строго в порядке запросов на соединении | M2 |
| CometBFT открывает **4 соединения** (consensus, mempool, query, snapshot) параллельно; `Engine` не потокобезопасен → один мьютекс вокруг всех вызовов `Engine` | M2 |
| `ResponseException` останавливает узел; ошибки в данных tx должны оставаться кодами результата | OK (ядро) |
| Методы `InsertTx`/`ReapTxs` (v0.40) вызываются только при `mempool.type = "app"`; используем `flood` | M2 (конфиг) |

## Жизненный цикл
| Метод | Требование | Статус |
|---|---|---|
| `Info` | вызывается первым всегда *(код)*; вернуть `last_block_height` и `last_block_app_hash` только из durable-метаданных | OK (`Engine::info`) |
| `InitChain` | вызывается при `last_block_height = 0`; `app_state_bytes` = наш genesis JSON; сверить chain_id, `initial_height = 1`, время, набор валидаторов (приходит отсортированным по power) | M2 (сверка — protocol.md §11.4) |
| `InitChain.app_hash` | непустой ответ заменяет app_hash состояния и становится `Header.AppHash` блока 1; поле `app_hash` в genesis CometBFT оставить **пустым** *(код)* | OK (protocol.md §11.4, r5) |
| `InitChain.validators` | вернуть пустой список (принять набор из genesis) | M2 |
| Крах после `InitChain` до первого `Commit` | приложение должно снова сообщать высоту 0 | OK (`init_chain` атомарен) |
| `CheckTx` | NEW / RECHECK; мемпул использует только `code` и `gas_wanted`; recheck после каждого Commit | OK (check-state сбрасывается на commit) |
| `PrepareProposal` | бюджет `max_tx_bytes` считает protobuf-обрамление каждой tx; превышение → паника CometBFT | OK (`proposal_tx_cost`, коммит 7a7cb7c) |
| `PrepareProposal` | может менять порядок/добавлять/выкидывать tx; не обязан быть детерминированным | OK |
| `ProcessProposal` | детерминированный, ACCEPT=1 / REJECT=2, UNKNOWN=0 роняет узел; вызывается и на proposer | OK (status маппинг — M2) |
| `ProcessProposal` | контекстные ошибки у нас — fail-stop; безопасно, т.к. CometBFT валидирует блок (LastCommit, время) до вызова | CHECK |
| Vote extensions | выключены по умолчанию (`abci.vote_extensions_enable_height = 0`); не включать | M2 (genesis) |
| `FinalizeBlock` | `decided_last_commit`: пуст на `initial_height`; иначе по одной записи на каждого валидатора H−1, порядок — по убыванию power *(код)*; Absent присутствуют с флагом 1 | OK (мы сортируем по адресу, §9.5) |
| `FinalizeBlock` | `len(tx_results) == len(txs)` иначе `ApplyBlock` падает | OK |
| `FinalizeBlock` | `app_hash` возвращается **здесь** (не в Commit), попадает в заголовок H+1 | OK |
| `BlockIDFlag` | UNKNOWN=0, ABSENT=1, COMMIT=2, NIL=3 | OK (§9.1) |
| `last_results_hash` | детерминированные поля: `code`, `data`, `gas_wanted`, `gas_used`; `codespace`, `log`, events не хешируются *(код)* | OK (§9.4, r5) |
| `Commit` | до ответа состояние должно быть durable; `retain_height` = 0 (не прунить) | OK (`Engine::commit`) / M2 |
| Рестарт | app < store → повтор FinalizeBlock+Commit для недостающих высот; app > store → `ErrAppBlockHeightTooHigh` | OK (повтор высоты = fail-stop без двойной эмиссии) |
| Validator updates | `ValidatorUpdate{pub_key, power}`, вступают в силу на H+2; до governance (ADR-0013) не используются | позже |

## Время и блоки
| Пункт | Статус |
|---|---|
| BFT time: взвешенная медиана меток precommit; в v0.40 Nil/Absent пропускаются; первый блок — время genesis | OK (§9.2 допускает равенство на высоте 1) |
| `Timestamp{int64 seconds, int32 nanos}` → `time_ms` по §9.1 (с проверкой диапазона) | M2 |
| Интервал блоков: `[consensus] timeout_commit`, `create_empty_blocks` в config.toml (параметра `next_block_delay` в v0.40 нет) | M2 (конфиг) |
| `BlockParams.MaxBytes` по умолчанию ≈ 53 МиБ в v0.40 → блоки > 100 000 tx возможны | OK (лимит записи результатов снят, коммит 7123625) |

## Непроверенное
- Выйдет ли v0.40.1 до старта M2.
- Дата окончания поддержки семейства 2025.1 (v0.38).
- Поведение на живом узле: всё, что помечено CHECK, и полный цикл на 4 валидаторах.
