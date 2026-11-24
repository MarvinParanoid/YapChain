# ADR-0028: ABCI++ адаптер `yap_abci_app`

**Статус:** DECIDED (M2.2) · опирается на [ADR-0026](0026-cometbft-version.md), [ADR-0027](0027-cometbft-acquisition.md),
[ADR-0022](0022-storage-layout.md), [ADR-0025](0025-dev-chain.md), protocol.md §9–§11, чек-лист
[cometbft-compatibility.md](../cometbft-compatibility.md)

## Контекст
CometBFT v0.40.0 открывает к приложению четыре соединения (consensus, mempool, query, snapshot) и вызывает методы
параллельно. `chain::Engine` не потокобезопасен и не знает о protobuf. Нужно соединить их, не меняя ядро и не
допуская ни одного ответа, который выглядел бы как успех после внутренней ошибки.

## Решение
1. **Слои.** `yap_abci` — транспорт (кадры, Unix socket, M2.1). `yap_abci_app` — `abci::Application`
   (реализация `Handler`) и чистые конверсии `convert.hpp` (protocol.md §9.1). Ядро не изменено.
2. **Синхронизация — один мьютекс** на весь `Application::handle`: каждый запрос любого соединения выполняется
   целиком под ним. Engine никогда не используется параллельно; порядок операций = порядок захвата мьютекса.
   CheckTx/PrepareProposal/ProcessProposal/Query работают на копиях или committed-данных и не меняют state.
3. **Fail-stop залипает.** Ошибки данных транзакций — коды результата; ошибочный запрос Query — код
   `yap.query`. Всё остальное (несогласованный вход консенсуса, отказ хранилища, неподдерживаемый метод) —
   исключение → транспорт отвечает `ResponseException`, закрывает соединение, вызывает `on_fatal`; CometBFT
   останавливается. После первой такой ошибки **каждый** следующий запрос тоже завершается исключением.
4. **Info** сообщает `last_block_height`/`last_block_app_hash` только из durable-метаданных. При высоте 0 (в том
   числе «InitChain выполнен, блоков нет») — высота 0 и пустой hash; CometBFT в этом случае снова шлёт InitChain
   (`consensus/replay.go`: InitChain вызывается при любом `appBlockHeight == 0`).
5. **InitChain идемпотентен для того же genesis.** Проверки §11.4: chain_id, `initial_height = 1`, время в мс,
   набор валидаторов как множество (pubkey Ed25519 32 байта, power > 0), `app_state_bytes` — наш genesis JSON;
   дополнительно `consensus_params` обязательны, `abci.vote_extensions_enable_height = 0`,
   `version.app = 1` (= `app_version` из Info, см. п. 6). Если store уже инициализирован на высоте 0 тем же
   genesis (`genesis_hash` совпадает), возвращается сохранённый root; иной genesis или наличие блоков — fail-stop.
   Ответ: `app_hash` = state root, `validators` пуст, `consensus_params` не возвращаются.
6. **`app_version = 1`** — версия прикладного протокола (protocol.md r5). Genesis CometBFT обязан содержать
   `consensus_params.version.app = "1"`, иначе версия в заголовках блоков и в параметрах разошлась бы
   (CometBFT при высоте 0 берёт её из Info).
7. **FinalizeBlock** выполняет решённый блок заново всегда; если в памяти есть незакоммиченный блок (CometBFT
   перезапустился до Commit, а `yapd` — нет), он отбрасывается и блок исполняется повторно — ничего из него не
   было записано. `app_hash` возвращается здесь; `tx_results` — ровно по одному на tx, `data` пусто, `gas_* = 0`.
8. **Commit** отвечает только после durable-записи всей высоты (`Engine::commit`); `retain_height = 0`.
9. **CheckTx** NEW и RECHECK — одна операция над check-state, который сбрасывается каждым Commit; CometBFT
   перепроверяет оставшиеся tx мемпула по порядку.
10. **Query** (не консенсус): `/chain`, `/params`, `/account/<addr>`; ответ — JSON (u64 — десятичные строки);
    только последняя закоммиченная высота (`height` = 0 или равна ей); `prove = true` → код «не поддерживается»
    (поддельных доказательств нет). Коды: 1 неизвестный путь, 2 неверный запрос, 3 высота недоступна,
    4 доказательства не поддерживаются, 5 цепочка не инициализирована.
11. **Неподдерживаемое — явно.** ListSnapshots — пусто, OfferSnapshot — REJECT, LoadSnapshotChunk — пусто,
    ApplySnapshotChunk — ABORT; ExtendVote — пустое расширение, VerifyVoteExtension — ACCEPT только пустого;
    InsertTx/ReapTxs (`mempool.type = "app"`) — исключение (fail-stop): используем `flood`.
12. **Dev-chain и ABCI не делят базу.** `Application` отказывается работать со store, где есть конфигурация dev-chain
    (`yapchain/dev`); `DevChain` отказывается работать со store без неё. Одновременную запись двумя процессами
    исключает `flock` на `yap.lock` (ADR-0022, M1.5).

## Последствия
- Пропускная способность ограничена одним мьютексом: Query/CheckTx ждут FinalizeBlock. Для testnet достаточно;
  оптимизация (снимки для чтения вне мьютекса) — позже и только с отдельными тестами TSan.
- Изменение `app_version` в будущем потребует обновления через `ConsensusParams` и отдельного ADR.
