# Чек-лист совместимости с CometBFT v0.40.0 (для M2)

**M2.0 (24.11.2026):** версия перепроверена по тегам и коду — патч-релизов нет, официальных бинарников нет, критичный
advisory Tachyon в v0.40.0 исправлен, номера полей `Request`/`Response` и поля сообщений ниже сверены с
`proto/tendermint/abci/types.proto` тега; получение и сборка — [ADR-0027](adr/0027-cometbft-acquisition.md).

Статусы: **OK** — ядро уже соответствует; **M2** — сделать в адаптере ABCI; **CHECK** — проверить на живом узле;
**M2.1**/**M2.2** — реализовано и покрыто тестами в транспорте (`yap_abci`) / адаптере (`yap_abci_app`, ADR-0028).
Источники: proto и код на теге v0.40.0 (`proto/tendermint/abci/types.proto`, `abci/types/messages.go`,
`state/execution.go`, `consensus/replay.go`, `abci/client/socket_client.go`), спецификация v0.38.26 `spec/abci/`,
сайт docs.cosmos.network/cometbft/latest. Пункты, подтверждённые только кодом, помечены *(код)*.

## Транспорт
| Пункт | Статус |
|---|---|
| Кадр: protobuf `Request`/`Response`, префикс длины — **unsigned varint** (`binary.PutUvarint`), максимум `MaxInt32` | M2.1 (лимит 128 МиБ) |
| Номера полей `oneof` в `Request` и `Response` **различаются** (например, finalize_block = 20 / 21) | M2.1 (код сгенерирован официальным protoc из proto тега) |
| `Flush`: ответить `ResponseFlush` и сбросить буфер; ответы — строго в порядке запросов на соединении | M2.1 |
| CometBFT открывает **4 соединения** (consensus, mempool, query, snapshot) параллельно; `Engine` не потокобезопасен → один мьютекс вокруг всех вызовов `Engine` | M2.2 (TSan) |
| `ResponseException` останавливает узел; ошибки в данных tx должны оставаться кодами результата | OK (ядро) |
| Методы `InsertTx`/`ReapTxs` (v0.40) вызываются только при `mempool.type = "app"`; используем `flood` | M2.2 (исключение) / конфиг — M2.3 |

## Жизненный цикл
| Метод | Требование | Статус |
|---|---|---|
| `Info` | вызывается первым всегда *(код)*; вернуть `last_block_height` и `last_block_app_hash` только из durable-метаданных | OK (`Engine::info`) |
| `InitChain` | вызывается при `last_block_height = 0`; `app_state_bytes` = наш genesis JSON; сверить chain_id, `initial_height = 1`, время, набор валидаторов (приходит отсортированным по power) | M2.2; InitChain идемпотентен для того же genesis — CometBFT шлёт его при каждом `appBlockHeight = 0` *(код)* |
| `InitChain.app_hash` | непустой ответ заменяет app_hash состояния и становится `Header.AppHash` блока 1; поле `app_hash` в genesis CometBFT оставить **пустым** *(код)* | OK (protocol.md §11.4, r5) |
| `InitChain.validators` | вернуть пустой список (принять набор из genesis) | M2.2 |
| Крах после `InitChain` до первого `Commit` | приложение должно снова сообщать высоту 0 | OK (`init_chain` атомарен) |
| `CheckTx` | NEW / RECHECK; мемпул использует только `code` и `gas_wanted`; recheck после каждого Commit | OK (check-state сбрасывается на commit); RECHECK под `mempool.Lock` до любого нового CheckTx *(код)*; живой RECHECK — M2.4 |
| `PrepareProposal` | бюджет `max_tx_bytes` считает protobuf-обрамление каждой tx; превышение → паника CometBFT | OK (`proposal_tx_cost`, коммит 7a7cb7c) |
| `PrepareProposal` | может менять порядок/добавлять/выкидывать tx; не обязан быть детерминированным | OK |
| `ProcessProposal` | детерминированный, ACCEPT=1 / REJECT=2, UNKNOWN=0 роняет узел; вызывается и на proposer | OK; маппинг — M2.2 |
| `ProcessProposal` | контекстные ошибки у нас — fail-stop; безопасно, т.к. CometBFT валидирует блок (LastCommit, время) до вызова | M2.4: порядок `ValidateBlock` → `ProcessProposal` сверен с `consensus/state.go`; невалидные tx → REJECT (12 видов), живые чужие предложения — ACCEPT (ADR-0028 п. 14) |
| Vote extensions | выключены по умолчанию (`abci.vote_extensions_enable_height = 0`); не включать | M2.2 (InitChain отказывает при ≠ 0) |
| `FinalizeBlock` | `decided_last_commit`: пуст на `initial_height`; иначе по одной записи на каждого валидатора H−1, порядок — по убыванию power *(код)*; Absent присутствуют с флагом 1 | OK (мы сортируем по адресу, §9.5) |
| `FinalizeBlock` | `len(tx_results) == len(txs)` иначе `ApplyBlock` падает | OK |
| `FinalizeBlock` | `app_hash` возвращается **здесь** (не в Commit), попадает в заголовок H+1 | OK |
| `BlockIDFlag` | UNKNOWN=0, ABSENT=1, COMMIT=2, NIL=3 | OK (§9.1) |
| `last_results_hash` | детерминированные поля: `code`, `data`, `gas_wanted`, `gas_used`; `codespace`, `log`, events не хешируются *(код)* | OK (§9.4, r5) |
| `Commit` | до ответа состояние должно быть durable; `retain_height` = 0 (не прунить) | OK (`Engine::commit`) / M2.2 |
| Рестарт | app < store → повтор FinalizeBlock+Commit для недостающих высот; app > store → `ErrAppBlockHeightTooHigh` | OK (повтор высоты = fail-stop без двойной эмиссии) |
| Validator updates | `ValidatorUpdate{pub_key, power}`, вступают в силу на H+2; до governance (ADR-0013) не используются | позже |

## Время и блоки
| Пункт | Статус |
|---|---|
| BFT time: взвешенная медиана меток precommit; в v0.40 Nil/Absent пропускаются; первый блок — время genesis | OK (§9.2 допускает равенство на высоте 1) |
| `Timestamp{int64 seconds, int32 nanos}` → `time_ms` по §9.1 (с проверкой диапазона) | M2.2 |
| Интервал блоков: `[consensus] timeout_commit`, `create_empty_blocks` в config.toml (параметра `next_block_delay` в v0.40 нет) | M2.3 (конфиг) |
| `BlockParams.MaxBytes` по умолчанию ≈ 53 МиБ в v0.40 → блоки > 100 000 tx возможны | OK (лимит записи результатов снят, коммит 7123625) |

## Номера полей (сверено с proto тега v0.40.0)
- `Request`: echo 1, flush 2, info 3, init_chain 5, query 6, check_tx 8, commit 11, list_snapshots 12, offer_snapshot 13,
  load_snapshot_chunk 14, apply_snapshot_chunk 15, prepare_proposal 16, process_proposal 17, extend_vote 18,
  verify_vote_extension 19, finalize_block 20, insert_tx 21, reap_txs 22 (reserved 4, 7, 9, 10).
- `Response`: exception 1, echo 2, flush 3, info 4, init_chain 6, query 7, check_tx 9, commit 12, list_snapshots 13,
  offer_snapshot 14, load_snapshot_chunk 15, apply_snapshot_chunk 16, prepare_proposal 17, process_proposal 18,
  extend_vote 19, verify_vote_extension 20, finalize_block 21, insert_tx 22, reap_txs 23 (reserved 5, 8, 10, 11).
- `RequestFinalizeBlock`: txs 1, decided_last_commit 2, misbehavior 3, hash 4, height 5, time 6, next_validators_hash 7,
  proposer_address 8; `ResponseFinalizeBlock`: events 1, tx_results 2, validator_updates 3, consensus_param_updates 4,
  app_hash 5; `CommitInfo{round 1, votes 2}`; `VoteInfo{validator 1, block_id_flag 3}`; `ResponseCommit{retain_height 3}`;
  `ResponseInfo{data 1, version 2, app_version 3, last_block_height 4, last_block_app_hash 5}`.

## Поведение методов, которые ядро не поддерживает (M2.2)
| Метод | Ответ |
|---|---|
| `ListSnapshots` | пустой список (снапшотов нет) |
| `OfferSnapshot` | `REJECT` (state sync не поддерживается, ADR-0006/consensus.md §7) |
| `LoadSnapshotChunk` | пустой chunk |
| `ApplySnapshotChunk` | `ABORT` |
| `ExtendVote` / `VerifyVoteExtension` | не вызываются при `vote_extensions_enable_height = 0`; если вызваны — пустое расширение / `ACCEPT` только для пустого, иначе `REJECT` |
| `InsertTx` / `ReapTxs` | используются только при `mempool.type = "app"`; мы не включаем; при вызове — `ResponseException` (явный отказ) |

## `app_version` (M2.2)
`Info.app_version = 1`; genesis CometBFT обязан содержать `consensus_params.version.app = "1"` (InitChain проверяет):
CometBFT берёт версию из Info только при высоте 0, а из параметров — при их обновлении (ADR-0028 п. 6).

## Результаты на живом узле (M2.3, 24.11.2026)
CometBFT v0.40.0 (собран из архива тега, `0.39.0+0880b4d3`) + `yapd`, один валидатор, Linux x86_64, GCC 16 Debug,
ASan+UBSan и Clang 23 TSan; данные — из `--trace-abci` yapd, лога и RPC CometBFT (`tests/integration/test_live_cometbft.py`).

| Пункт | Наблюдение | Итог |
|---|---|---|
| Соединения | 4 socketClient: query, snapshot, mempool, consensus | OK |
| Порядок старта | `Info` (height 0, hash пуст) → `InitChain` → блоки | OK |
| `InitChain` | `initial_height=1`, 1 валидатор, `version.app=1`, `vote_extensions_enable_height=0`; все проверки §11.4 пройдены; `app_hash` = root CLI-genesis | OK |
| `Header(1).AppHash` | = `app_hash` ответа InitChain (поле `app_hash` genesis пусто) | OK |
| Время | время блока 1 = `genesis_time`; дальше — BFT time, растёт | OK |
| `decided_last_commit` | высота 1 — пусто; с высоты 2 — 1 голос, flag 2 (COMMIT), адрес и power совпадают с state | OK |
| `PrepareProposal` | `max_tx_bytes = 55 596 101`; `local_last_commit` с 1 голосом; затем `ProcessProposal` на том же узле | OK |
| `app_hash` | `Header(H+1).AppHash` = root высоты H для **всех** высот прогона; `/chain` Query = заголовку | OK |
| `version.app` в заголовках | 1 | OK |
| CheckTx | ERR_NONCE (10), ERR_DECODE (1), ERR_INSUFFICIENT_FUNDS (11) возвращаются через `broadcast_tx_sync`; точный повтор тех же байт отклоняет **кэш мемпула CometBFT** («tx already exists in cache»), до приложения он не доходит | OK |
| RECHECK | не наблюдался: после Commit мемпул был пуст; покрыт тестом адаптера | — |
| Рестарт | `Info` → height/hash последнего Commit; `appHeight = storeHeight = stateHeight`; InitChain не вызывается; первый новый блок несёт тот же `app_hash` | OK |
| Сбой до Commit (`before_commit:H`) | durable высота H−1; CometBFT: «consensus connection terminated. Did the application crash?», останавливается сам (код 0); после рестарта `appHeight=H−1 storeHeight=H stateHeight=H−1` → «Replay last block using real app» | OK |
| Сбой после Commit (`after_commit:H`) | durable высота H; после рестарта `appHeight=H storeHeight=H stateHeight=H−1` → «Replay last block using mock app» (повторного исполнения нет) | OK |
| Двойное применение | по логу yapd каждая высота закоммичена ровно один раз (1..N без пропусков); эмиссия точно `G + 2 YAP × H` | OK |

## Результаты сети из четырёх валидаторов (M2.4, 24.11.2026)
Четыре независимых валидатора: у каждого свой процесс CometBFT v0.40.0 и свой `yapd` со своей LMDB, сокетом, портами,
validator key, node key и reward-счётом; P2P только через CometBFT на 127.0.0.1, state sync выключен, mempool `flood`.
Linux x86_64; GCC 16 Debug и ASan+UBSan, Clang 23 TSan; 5 отдельных прогонов Debug с новыми ключами
(`tests/integration/test_live_multival.py`). Значения сравниваются строго по высоте.

| Пункт | Наблюдение | Итог |
|---|---|---|
| Genesis | один `genesis.json`, побайтно одинаковый у всех нод; InitChain на всех четырёх, одинаковые `genesis_hash` и `app_hash` | OK |
| P2P | у каждой ноды 3 пира (persistent peers, `allow_duplicate_ip`, без PEX) | OK |
| Proposer | за высоты 1..30 каждый из 4 валидаторов предлагал 7–8 блоков (round-robin при равной power) | OK |
| `ProcessProposal` | вызывается на каждой ноде для каждого блока, включая свои; ~23 из 31 — предложения **других** валидаторов; все ACCEPT | OK |
| Детерминизм | block hash, `Header.AppHash`, `block_results` и state root совпадают на всех 4 нодах на каждой общей высоте; `Header(H+1).AppHash` = root(H) каждой ноды | OK |
| Транзакции через RPC разных нод | Transfer/Burn через валидаторы 0–3; распространение по P2P (CheckTx NEW того же хеша на других нодах); одинаковые высота и код на всех нодах | OK |
| Одни байты на 3 ноды | приняты или отклонены кэшем CometBFT, исполнены один раз; повтор после включения отклоняется кэшем | OK |
| Конфликт одного nonce | обе ноды приняли свою tx (0/0); исполнена ровно одна (в прогонах выигрывали обе стороны), проигравшая удалена из всех мемпулов | OK |
| RECHECK | живой RECHECK на всех нодах (3–12 за прогон); валидные tx — код 0; проигравшая конфликт — код 0 до Commit победителя и ERR_NONCE (10) после, затем удаляется; зависших tx нет | OK |
| `decided_last_commit` | у приложения — те же адреса, power 10 и флаги, что в LastCommit блока (подписи сопоставляются с набором H−1 по индексу: у ABSENT адрес пуст) | OK |
| Награды | 300 000 yip каждому из 4 подписантов, 400 000 — из 3; пул 600 000, казна 200 000 + остаток; высота 1 — доля валидаторов в казну; совпадает до yip на всех нодах | OK |
| ABSENT | после остановки validator-3 — ABSENT в каждом следующем LastCommit; награды ему нет; трое продолжают производить блоки | OK |
| NIL | наблюдается вживую (3–7 раз за прогон): валидатор получает +2/3 precommit раньше +2/3 prevote и по правилам CometBFT голосует precommit nil (`no +2/3 prevotes during enterPrecommit`; debug-лог консенсуса). Награды за эту высоту нет | OK (см. риски) |
| Blocksync | validator-3 стоит ≥ 20 блоков, затем стартует со своими данными: handshake на высоте остановки, 21–23 блока применены через blocksync (FinalizeBlock без ProcessProposal), затем `Time to switch to consensus mode!` | OK |
| После догона | те же хеши и roots, баланс/nonce/supply точные, снова голосует COMMIT, предлагает блок, обрабатывает чужие предложения; каждая высота закоммичена один раз | OK |
| verify-state | после корректной остановки — у всех четырёх баз; root совпадает с root той же высоты у других нод | OK |
| Секреты | пароль кошелька и приватные ключи (validator, node) не встречаются ни в одном логе (trace включён) | OK |

## Непроверенное
- Выйдет ли v0.40.1 до старта M2 (на 24.11.2026 — нет).
- Дата окончания поддержки семейства 2025.1 (v0.38).
- Misbehavior на живой сети: Byzantine-поведение не воспроизводилось (вне scope M2.4); обработка поля покрыта тестами
  адаптера (ADR-0028 п. 15).
- Разделение сети и потеря кворума проверены в M2.5 ([fault-tolerance.md](fault-tolerance.md)); state sync и
  изменение набора валидаторов — последующие этапы.
- Работающая нода после восстановления связи догоняет через catch-up gossip консенсуса, а не через blocksync
  (`blocksync.adaptive_sync` выключен; включать не стали) — M2.5, §3 fault-tolerance.md.
- aarch64 и GitHub Actions — не запускались.
