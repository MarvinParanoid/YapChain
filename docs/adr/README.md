# Architecture Decision Records

Формат: контекст → решение → альтернативы → последствия. Статус ADR — **DECIDED**, **PROPOSED** или **OPEN**.
Принятые ADR не переписываются (меняется только колонка статуса в этом индексе): изменение решения оформляется новым ADR со ссылкой «заменяет ADR-XXXX».

| ADR | Тема | Статус |
|---|---|---|
| [0001](0001-record-decisions.md) | Ведение решений и статусов | DECIDED |
| [0002](0002-consensus-engine.md) | CometBFT через ABCI++ как консенсус-движок | DECIDED через ADR-0012 |
| [0003](0003-canonical-serialization.md) | Каноническая сериализация YCS | PROPOSED |
| [0004](0004-cryptography.md) | Ed25519 (libsodium), SHA-256, tagged hashing | PROPOSED, уточнён ADR-0014 |
| [0005](0005-addresses-bech32m.md) | Адреса: хеш ключа + Bech32m | PROPOSED |
| [0006](0006-state-commitment-smt.md) | State root: Sparse Merkle Tree | PROPOSED |
| [0007](0007-storage-lmdb.md) | Хранилище состояния: LMDB | PROPOSED |
| [0008](0008-achievement-rewards-from-pool.md) | Награды за достижения — перевод из пула, не mint | PROPOSED |
| [0009](0009-p2p-via-cometbft.md) | P2P-сеть на базе CometBFT вместо собственного стека | DECIDED через ADR-0012 |
| [0010](0010-strict-block-validity.md) | Строгая проверка блоков и детерминированный пропуск tx | PROPOSED |
| [0011](0011-governance-admin-ops.md) | Управление реестрами, валидаторами и Treasury | закрыт ADR-0013 |
| [0012](0012-owner-decisions-after-m0.md) | Решения владельца проекта после M0 | DECIDED |
| [0013](0013-governance-multisig.md) | Governance — multisig 3-of-5 (после M1) | DECIDED |
| [0014](0014-ed25519-verification-rules.md) | Нормативный алгоритм проверки Ed25519 | DECIDED |
| [0015](0015-validator-rewards-last-commit.md) | Награды валидаторам и семантика last_commit | DECIDED |
| [0016](0016-genesis-single-source.md) | Genesis — единый источник в app_state CometBFT | DECIDED |
| [0017](0017-checked-arithmetic-and-atomicity.md) | Checked-арифметика, классы ошибок, атомарность tx | DECIDED |
| [0018](0018-dependency-pinning.md) | Фиксация зависимостей через CMake (URL + SHA-256) | DECIDED |
| [0019](0019-normative-check-order.md) | Нормативный порядок проверок транзакций | DECIDED |
| [0020](0020-validator-public-key-validation.md) | Строгая проверка публичных ключей валидаторов | DECIDED |
