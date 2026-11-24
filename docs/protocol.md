# Спецификация протокола YapChain v1

Статусы: **DECIDED** — утверждено, **PROPOSED** — предлагается, **OPEN** — требует решения.
Если раздел не помечен отдельно, его статус — **PROPOSED**.

Документ нормативный: реализация на любом языке, которая следует ему, должна получать те же байты,
хеши, коды результатов и state root. Слова «ДОЛЖЕН», «НЕ ДОЛЖЕН», «МОЖЕТ» используются в смысле RFC 2119.

История ревизий:
- r1 (M0) — первая редакция.
- r2 (M0 finalization) — точный алгоритм проверки Ed25519 ([ADR-0014](adr/0014-ed25519-verification-rules.md)),
  классы арифметических ошибок и атомарность tx ([ADR-0017](adr/0017-checked-arithmetic-and-atomicity.md)),
  семантика `last_commit` ([ADR-0015](adr/0015-validator-rewards-last-commit.md)), genesis через `app_state`
  ([ADR-0016](adr/0016-genesis-single-source.md)), нормативный порядок проверок
  ([ADR-0019](adr/0019-normative-check-order.md)), уточнения `ProcessProposal`/`FinalizeBlock`.
- r3 — строгая проверка публичных ключей валидаторов (§5.7, §11.2 п. 4,
  [ADR-0020](adr/0020-validator-public-key-validation.md)). Правила проверки подписей (§5.2) не изменены.
- r4 — зачисление 0 yip не меняет state (§9.3); схема genesis JSON и выбор HRP (§11.3); недопустимый `flag` в
  `last_commit` — fail-stop (§9.1) ([ADR-0021](adr/0021-zero-credit-and-genesis-json.md)).
- r5 (аудит M1.5) — без изменений консенсусных форматов: закрыты OPEN-пункты §9.4 и §11.4 по CometBFT v0.40.0
  ([ADR-0026](adr/0026-cometbft-version.md)); уточнён бюджет `PrepareProposal` (§10); для dev-блоков — верхняя
  граница времени и лимиты размера при импорте (§14).

## 1. Обозначения и арифметика

- `u8, u16, u32, u64` — беззнаковые целые указанной ширины.
- `[u8; N]` — массив ровно из N байт.
- `bytes` — байтовая строка переменной длины, `string` — `bytes` с дополнительными ограничениями на содержимое.
- `vec<T>` — последовательность элементов.
- `||` — конкатенация байт.
- `U64_MAX = 2^64 − 1`.
- `floor(a × b / c)` (`mul_div_floor`) — точное целочисленное деление вниз; `c ≥ 1`; произведение `a × b` ДОЛЖНО
  вычисляться без переполнения (128-битная беззнаковая арифметика). Если результат не помещается в `u64` — ошибка
  переполнения.
- Все сложения и вычитания над балансами, supply, nonce, высотами и временем — **checked**: результат вне `[0, U64_MAX]`
  является ошибкой, wrap-around и насыщение запрещены (кроме мест, где явно сказано иное).

### 1.1 Классы арифметических ошибок (ADR-0017)

| Где произошла ошибка | Реакция |
|---|---|
| Внутри исполнения транзакции (валидация или эффекты) | tx получает `ERR_OVERFLOW`, все её изменения отбрасываются (§7.5) |
| Вне транзакций: эмиссия, обновление `ChainState`, genesis при `InitChain` | **fail-stop**: нода останавливается, блок не коммитится |
| Валидация genesis-файла до запуска | genesis невалиден, нода не стартует |

При соблюдении инвариантов §7.3 ошибки второго класса невозможны; их появление означает баг или порчу state,
и продолжать исполнение нельзя — иначе возможно расхождение нод.

## 2. Сеть и chain_id

| Поле | Значение |
|---|---|
| `chain_id` testnet | `yapchain-testnet-1` |
| `chain_id` mainnet | зарезервирован, OPEN |
| Формат | ASCII, символы `[a-z0-9-]`, длина 1..32, не начинается и не заканчивается на `-` |
| HRP адресов testnet | `tyap` |
| HRP адресов mainnet | `yap` (зарезервирован) |

`chain_id` входит в каждую подписываемую структуру (транзакции, аттестации, dev-блоки),
поэтому подпись, сделанная для одной сети, невалидна в другой.
При несовместимом перезапуске testnet номер увеличивается (`yapchain-testnet-2`).

## 3. Каноническая сериализация (YCS)

YCS (YapChain Canonical Serialization) — единственный формат, по которому считаются хеши и подписи.
Формат совпадает с подмножеством Borsh, но нормативна именно эта спецификация.

### 3.1 Правила кодирования

| Тип | Кодирование |
|---|---|
| `u8`, `u16`, `u32`, `u64` | фиксированная ширина, little-endian |
| `bool` | 1 байт: `0x00` или `0x01` |
| `[u8; N]` | N байт как есть, без длины |
| `bytes` | `u32` длина + байты |
| `string` | как `bytes`; содержимое ограничено правилом конкретного поля |
| `vec<T>` | `u32` количество элементов + элементы подряд |
| `option<T>` | `0x00` (нет) или `0x01` + `T` |
| `enum` | `u8` тег варианта + поля варианта |
| `struct` | поля подряд в порядке объявления, без выравнивания и без имён |

### 3.2 Правила строгого декодирования

Декодер ДОЛЖЕН отклонить вход, если:

1. Данные закончились раньше, чем структура декодирована.
2. После корневой структуры остались лишние байты.
3. `bool` не равен `0x00`/`0x01`; тег `option` не равен `0x00`/`0x01`.
4. Тег `enum` не определён в данной версии.
5. Длина `bytes`/`string`/`vec` превышает лимит поля (§3.3); лимит проверяется **до** выделения памяти.
   Длина `bytes`/`string` также не может превышать число оставшихся байт; число элементов `vec` — число оставшихся
   байт, делённое на минимальный размер элемента (защита от аллокаций по фиктивной длине).
6. Нарушено ограничение содержимого поля (charset, диапазон).

Следствие: у каждого значения ровно одно допустимое кодирование, и `encode(decode(b)) == b` для любого принятого `b`.

### 3.3 Лимиты полей переменной длины

У каждого поля `bytes`/`string`/`vec` в спецификации ДОЛЖЕН быть указан максимум. Декодер получает его как
параметр поля, а не из state; лимиты, зависящие от `ChainParams` (`memo`), проверяются при валидации (§6.2), а декодер
применяет их жёсткий потолок.

| Поле | Потолок декодера | Содержимое |
|---|---|---|
| любой `chain_id` | 32 | §2 |
| `TxBody.memo` | 1 024 | любые байты; фактический лимит — `max_memo_bytes` |
| `ValidatorInfo.name`, `Issuer.name` | 32 | ASCII `0x20..0x7E` |
| `Issuer.keys` | 8 элементов | — |
| `DevBlock.txs` | 10 000 элементов, каждый ≤ 65 536 байт | — |
| `Genesis.accounts` | 1 000 000 | — |
| `Genesis.validators` | 32 | — |
| `Genesis.issuers`, `Genesis.achievements` | 1 024, 65 536 | — |

## 4. Хеширование и domain separation

### 4.1 Хеш-функция
SHA-256 (FIPS 180-4) везде, где не указано иное. SHA-512 используется только внутри Ed25519.

### 4.2 Tagged hash

```
H_tag(tag, msg) = SHA-256( u8(len(tag)) || tag || msg )
```

`tag` — ASCII-строка длиной 1..255 байт. Длина-префикс делает разбиение `tag || msg` однозначным.

### 4.3 Реестр тегов хешей

| Тег | Назначение |
|---|---|
| `YAP/addr/v1` | account_id из публичного ключа |
| `YAP/sys-account/v1` | account_id системных аккаунтов |
| `YAP/txid/v1` | идентификатор транзакции |
| `YAP/state-key/v1` | путь ключа в SMT |
| `YAP/state-value/v1` | хеш значения в SMT |
| `YAP/smt/leaf/v1` | лист SMT |
| `YAP/smt/node/v1` | внутренний узел SMT |
| `YAP/genesis/v1` | хеш genesis |
| `YAP/nullifier/v1` | nullifier аттестации |
| `YAP/devblock/v1` | хеш dev-блока (M1) |

### 4.4 Префиксы подписей

Подпись Ed25519 ставится над сообщением `sign_prefix || YCS(структура)`.
Префикс — ASCII-строка с завершающим байтом `0x00`. Внутри префиксов нет `0x00`, поэтому множество префиксов
prefix-free, и подпись одного типа нельзя выдать за подпись другого.

| Префикс | Что подписывается | Кто подписывает |
|---|---|---|
| `YAPCHAIN/TX/V1\0` | `TxBody` | владелец аккаунта-отправителя |
| `YAPCHAIN/ATTESTATION/V1\0` | `Attestation` | ключ приложения-эмитента |
| `YAPCHAIN/DEVBLOCK/V1\0` | `DevBlockHeader` | dev-производитель блоков (только M1) |

Подписи CometBFT (голоса, предложения) используют собственный формат CometBFT и не пересекаются с этими сообщениями
(разные ключи: ключ валидатора — не ключ аккаунта; см. §5.5).

## 5. Ключи и адреса

### 5.1 Ed25519
- Алгоритм: Ed25519 по RFC 8032 (PureEdDSA, без prehash и без context).
- Приватный ключ — 32-байтовый seed из CSPRNG ОС (`randombytes_buf` libsodium).
- Публичный ключ — 32 байта, подпись — 64 байта. Подпись детерминирована (RFC 8032).

### 5.2 Алгоритм проверки подписи (нормативно, ADR-0014)

Обозначения: `p = 2^255 − 19`, `L = 2^252 + 27742317777372353535851937790883648493`, `B` — базовая точка RFC 8032.
Для 32-байтовой кодировки точки `E`: `y_raw(E)` — little-endian число из `E` с обнулённым старшим битом байта 31
(`0 ≤ y_raw < 2^255`), `sign(E)` — этот старший бит.

```
SMALL_Y = { 0,
            1,
            p − 1,
            2707385501144840649318225287225658788936804267575313519463743609750303402022,
            55188659117513257062467267217118295137698188065244968500265048394206261417927 }
```
(y-координаты всех 8 точек порядка, делящего 8.)

`verify(A_bytes, M, sig)`, где `R_bytes = sig[0..32]`, `S_bytes = sig[32..64]`:
1. `S` = little-endian число из `S_bytes`. Если `S ≥ L` → **reject**.
2. Если `y_raw(R_bytes) mod p ∈ SMALL_Y` → **reject**.
3. Если `y_raw(A_bytes) ≥ p` → **reject** (неканоническая кодировка ключа).
4. Если `y_raw(A_bytes) ∈ SMALL_Y` → **reject**.
5. Декодировать `A` по RFC 8032 §5.1.3; если `x` не существует → **reject**.
   (При `x = 0` бит знака не проверяется отдельно: такие точки уже отклонены шагом 4.)
6. `k = SHA-512(R_bytes || A_bytes || M) mod L`.
7. `R' = [S]B − [k]A`; принять, **если и только если** каноническая кодировка `R'` побайтно равна `R_bytes`.

Следствия: уравнение cofactorless; неканонический `R` отклоняется на шаге 7; ключ смешанного порядка (с ненулевой
кручёной компонентой, но не из `SMALL_Y`) **допускается**, если выполняется уравнение шага 7.
Это совпадает с поведением `crypto_sign_verify_detached` libsodium (сборка без `ED25519_COMPAT`), что подтверждается
векторами `test-vectors/ed25519.json`, сгенерированными независимой реализацией этого алгоритма (`tools/refgen`).
Обновление libsodium допускается только при прохождении этих векторов.

### 5.3 Адреса аккаунтов

```
account_id = H_tag("YAP/addr/v1", 0x00 || pubkey)        // 32 байта; 0x00 = алгоритм Ed25519
address    = Bech32m(hrp, [0] || convertbits_8_to_5(account_id, pad = true))
```

- Первый 5-битный символ данных — **версия адреса**, сейчас только `0`.
- Длина: `tyap` + `1` + 1 символ версии + 52 символа данных + 6 символов checksum = 64 символа (≤ 90, лимит BIP-350).
- Канонический вывод — нижний регистр.
- Разбор строки адреса:
  1. Общая проверка Bech32m по BIP-350: длина ≤ 90; символы `0x21..0x7E`; не смешанный регистр;
     последний `1` — разделитель; HRP 1..83 символа; часть данных ≥ 6 символов и только из алфавита Bech32;
     checksum с константой `0x2bc830a3` (Bech32 с константой `1` **отклоняется**).
  2. HRP (в нижнем регистре) равен HRP сети.
  3. Часть данных без checksum: ровно 53 символа, первый равен `0` (версия).
  4. `convertbits_5_to_8(остальные 52, pad = false)` даёт ровно 32 байта; оставшиеся 4 бита равны 0.
- В консенсусных структурах адрес всегда хранится как 32-байтовый `account_id`; Bech32m — только формат для людей.

Хеширование публичного ключа (а не использование его как адреса) оставляет место для других алгоритмов
(байт алгоритма входит в хеш).

### 5.4 Системные аккаунты

```
account_id = H_tag("YAP/sys-account/v1", name)
```

| name | Назначение |
|---|---|
| `treasury` | Treasury |
| `rewards-pool` | пул наград за достижения |

Для системных аккаунтов приватного ключа не существует: получить подпись от их имени можно только
через коллизию двух разных тегов SHA-256. Списания с них выполняет только state machine по правилам протокола.

### 5.5 Ключи валидаторов
Консенсусный ключ валидатора (Ed25519, управляется CometBFT) отделён от аккаунта для наград.
Связь `consensus_address → reward_account` задаётся в genesis (§11). Консенсусный адрес — формат CometBFT:
первые 20 байт `SHA-256(pubkey)` (обычный SHA-256, не tagged). Публичный ключ валидатора ДОЛЖЕН проходить
проверку §5.7.

### 5.6 Хранение ключей пользователей
- Keystore-файл: JSON `{version: 1, kdf: "argon2id", opslimit, memlimit, salt, nonce, ciphertext, address}`,
  шифрование seed — XChaCha20-Poly1305, ключ шифрования — Argon2id (`crypto_pwhash`, уровень MODERATE).
- Seed в памяти — в `sodium_malloc`, обнуляется `sodium_memzero`.
- Мнемоники (BIP-39) и HD-деривация — **OPEN**, не входят в M1; одна пара ключей = один адрес.

### 5.7 Проверка публичного ключа валидатора (нормативно, ADR-0020)

Применяется к консенсусным ключам валидаторов (genesis, а в будущем — операции изменения набора валидаторов).
**Не применяется** к ключам аккаунтов и эмитентов: для них действует только §5.2.

`is_valid_validator_key(A_bytes)` (обозначения §5.2):
1. Если `y_raw(A_bytes) ≥ p` → **invalid** (неканоническая кодировка).
2. Если `y_raw(A_bytes) ∈ SMALL_Y` → **invalid** (точка малого порядка; сюда же попадают кодировки с `x = 0`
   и установленным битом знака).
3. Декодировать `A` по RFC 8032 §5.1.3; если `x` не существует → **invalid**.
4. Если `[L]A ≠ O` (нейтральный элемент) → **invalid**: точка вне подгруппы простого порядка, т.е. имеет
   ненулевую кручёную компоненту.
5. Иначе — **valid**.

Это семантика `crypto_core_ed25519_is_valid_point()` libsodium; совпадение подтверждается векторами
`test-vectors/ed25519.json` (`validator_keys`), сгенерированными независимой реализацией (`tools/refgen`).
Разница с §5.2: ключ смешанного порядка допускается при проверке пользовательских подписей, но не как ключ валидатора.

## 6. Транзакции

### 6.1 Структуры

```
struct TxBody {
    version:           u16        // = 1
    chain_id:          string     // §2
    sender_pubkey:     [u8; 32]   // sender = account_id(sender_pubkey)
    nonce:             u64
    fee:               u64        // в yip
    expires_at_height: u64        // последняя высота, на которой tx может быть включена
    memo:              bytes      // §3.3; произвольные байты, не интерпретируются
    payload:           TxPayload
}

enum TxPayload {
    0x00 Transfer         { to: [u8; 32], amount: u64 }
    0x01 Burn             { amount: u64 }
    0x02 ClaimAchievement { attestation: SignedAttestation }   // см. attestations.md
    // 0x03..0x0F зарезервированы
    // 0x10..0x1F — административные операции multisig 3-of-5 (ADR-0013), после M1
}

struct SignedTx {
    body:      TxBody
    signature: [u8; 64]     // Ed25519(sender key, "YAPCHAIN/TX/V1\0" || YCS(body))
}
```

```
tx_bytes = YCS(SignedTx)                  // то, что передаётся в сети и попадает в блок
tx_id    = H_tag("YAP/txid/v1", tx_bytes)
```

Отправитель не указывается отдельным полем — он однозначно выводится из `sender_pubkey`,
поэтому несоответствие «адрес ≠ ключ» невозможно по построению.

### 6.2 Stateless-валидация

Проверки выполняются **строго в порядке номеров**; результат — код первой нарушенной проверки (ADR-0019).
Порядок нормативен, потому что код результата входит в консенсусный хеш результатов блока (§9.4).
Проверка подписи — последняя, как самая дорогая.

| # | Правило | Код |
|---|---|---|
| S1 | `len(tx_bytes) ≤ max_tx_bytes` | `ERR_TOO_LARGE` |
| S2 | строгое YCS-декодирование успешно, включая ограничения содержимого полей (§3) | `ERR_DECODE` |
| S3 | `version == 1` | `ERR_VERSION` |
| S4 | `chain_id` совпадает с chain_id сети | `ERR_CHAIN_ID` |
| S5 | `len(memo) ≤ max_memo_bytes` | `ERR_MEMO` |
| S6 | `fee ≥ min_fee` | `ERR_FEE` |
| S7 | `expires_at_height ≥ 1` | `ERR_EXPIRED` |
| S8 | Transfer: `amount ≥ 1`, `to ≠ sender` | `ERR_PAYLOAD` |
| S9 | Burn: `amount ≥ 1` | `ERR_PAYLOAD` |
| S10 | ClaimAchievement: stateless-правила аттестации (attestations.md §4.1) | `ERR_ATTESTATION` |
| S11 | подпись валидна по §5.2 | `ERR_SIGNATURE` |

Неизвестный тег payload — это ошибка декодирования (S2). `min_fee`, `max_tx_bytes`, `max_memo_bytes` берутся из
`ChainParams` в state (§12); они фиксированы genesis до появления административных операций.

### 6.3 Stateful-валидация
Выполняется после stateless против текущего state на высоте исполнения `H` (`H` — высота блока, в который
включается tx), также строго по порядку номеров.

| # | Правило | Код |
|---|---|---|
| T1 | `H ≤ expires_at_height` | `ERR_EXPIRED` |
| T2 | `expires_at_height − H ≤ max_tx_ttl_blocks` (разность корректна благодаря T1) | `ERR_TTL_TOO_LONG` |
| T3 | `nonce == account(sender).nonce` (у несуществующего аккаунта nonce = 0) | `ERR_NONCE` |
| T4 | `nonce < U64_MAX` | `ERR_OVERFLOW` |
| T5 | Transfer, Burn: `amount + fee` не переполняет `u64` | `ERR_OVERFLOW` |
| T6 | Transfer, Burn: `balance(sender) ≥ amount + fee` | `ERR_INSUFFICIENT_FUNDS` |
| T7 | ClaimAchievement: правила attestations.md §4.2 (A1–A10, по порядку) | `ERR_CLAIM_*`, `ERR_INSUFFICIENT_FUNDS` |

### 6.4 Nonce и защита от повторов
- Nonce — последовательный счётчик аккаунта, начинается с 0, увеличивается на 1 после **каждой успешно исполненной**
  транзакции отправителя. Пропуски невозможны: tx с `nonce > account.nonce` невалидна.
- Повтор той же tx невозможен: после исполнения nonce уже другой.
- Повтор в другой сети невозможен: `chain_id` подписан.
- Аккаунт с `nonce > 0` **никогда не удаляется** из state, даже при нулевом балансе, иначе nonce
  сбросится в 0 и старые транзакции снова станут валидными.
- `expires_at_height` ограничивает «висящие» транзакции; верхняя граница `max_tx_ttl_blocks` не даёт обойти это
  ограничение огромным значением.

### 6.5 Комиссии
Комиссия `fee` списывается с отправителя при успешном исполнении и распределяется по §9.3.
Приоритизации по комиссии нет: порядок в блоке определяет proposer.

### 6.6 Административные операции — DECIDED: multisig 3-of-5, реализация после M1
Изменение реестра эмитентов, набора валидаторов, параметров и расходы Treasury выполняются транзакциями с тегами
`0x10..0x1F`, подписанными 3 из 5 ключей governance-группы ([ADR-0013](adr/0013-governance-multisig.md)).
Формат будет специфицирован отдельной ревизией протокола. До этого перечисленное задаётся только genesis.

## 7. Состояние

### 7.1 Ключи state
`raw_key = namespace (u8) || YCS(идентификатор)`. Значение хранится как `YCS(value)`.

| ns | Идентификатор | Значение | Описание |
|---|---|---|---|
| `0x00` | — (пусто) | `ChainState` | глобальное состояние, ровно один ключ |
| `0x01` | `account_id: [u8;32]` | `AccountState` | аккаунты |
| `0x02` | — | `ChainParams` | параметры (§12), ровно один ключ |
| `0x03` | `consensus_address: [u8;20]` | `ValidatorInfo` | валидаторы |
| `0x04` | `issuer_id: u32` | `Issuer` | эмитенты аттестаций |
| `0x05` | `issuer_id: u32` | `IssuerUsage` | расход бюджета эмитента в эпохе |
| `0x06` | `issuer_id: u32, achievement_id: u32` | `Achievement` | каталог достижений |
| `0x07` | `subject: [u8;32], issuer_id: u32, achievement_id: u32` | `ClaimCounter` | счётчик claim'ов пользователя |
| `0x08` | `subject: [u8;32]` | `SubjectUsage` | общий лимит claim'ов пользователя в эпохе |
| `0x09` | `nullifier: [u8;32]` | `NullifierRecord` | использованные аттестации |

### 7.2 Структуры значений

```
struct ChainState {
    height:        u64    // последняя исполненная высота
    last_time_ms:  u64
    total_supply:  u64    // = total_minted − total_burned
    total_minted:  u64    // genesis + вся эмиссия
    total_burned:  u64
}

struct AccountState { balance: u64, nonce: u64 }

struct ValidatorInfo {
    name:             string    // ASCII 0x20..0x7E, 1..32
    consensus_pubkey: [u8; 32]
    power:            u64       // ≥ 1
    reward_account:   [u8; 32]
}
```

Структуры эмитентов, достижений и счётчиков — в [attestations.md §3](attestations.md#3-форматы).

### 7.3 Инварианты (проверяются тестами; в debug-сборке — после каждого блока)
1. `Σ AccountState.balance (по всем аккаунтам, включая системные) == total_supply`.
2. `total_supply == total_minted − total_burned`.
3. `total_minted ≤ max_issuance`.
4. Существует ровно по одному ключу `0x00` и `0x02`.

Из 1–3 следует, что любой баланс и любая их сумма ≤ `max_issuance` ≤ `U64_MAX`.

### 7.4 Создание аккаунтов
Аккаунт создаётся неявно при первом зачислении (Transfer, награда, эмиссия). Отправка с несуществующего аккаунта
невозможна (баланс 0 < fee), за исключением ClaimAchievement, где комиссия может быть оплачена из награды
(attestations.md §4.3). Удаление аккаунтов не предусмотрено.

### 7.5 Атомарность исполнения транзакции (ADR-0017)
Каждая транзакция исполняется в отдельном буфере изменений (overlay) поверх state блока:
- все чтения видят эффекты предыдущих успешных tx этого блока;
- при любой ошибке валидации или арифметики overlay отбрасывается целиком — state ровно такой, как до tx;
- при успехе overlay сливается в state блока.
Реализация НЕ ДОЛЖНА выполнять частичные записи в обход overlay. Ошибка доступа к хранилищу во время исполнения tx —
не ошибка tx, а fail-stop.

## 8. Commitments: Merkle roots

### 8.1 State root — Sparse Merkle Tree
Двоичное разреженное дерево глубины 256, ключ — путь, биты ключа берутся от старшего бита первого байта.

```
smt_key    = H_tag("YAP/state-key/v1", raw_key)
value_hash = H_tag("YAP/state-value/v1", YCS(value))
```

Хеш поддерева определяется рекурсивно:
- поддерево без листьев: `EMPTY = 32 нулевых байта`;
- поддерево ровно с одним листом `(k, v)`: `H_tag("YAP/smt/leaf/v1", k || value_hash(v))` — независимо от глубины;
- иначе: `H_tag("YAP/smt/node/v1", hash(left) || hash(right))`.

`state_root` — хеш корня. Пустое дерево → `EMPTY`.

Корень зависит только от множества пар (ключ, значение), а не от порядка вставки и реализации; поддерживаются
доказательства включения и невключения; обновление одного ключа — O(log n) хешей.

### 8.2 Root списка транзакций
Для dev-блоков (§14): Merkle root по RFC 6962 (SHA-256, лист `SHA-256(0x00 || tx_bytes)`,
узел `SHA-256(0x01 || l || r)`, разбиение `n` листьев на `k` и `n − k`, где `k` — наибольшая степень двойки `< n`;
пустой список — `SHA-256("")`). В режиме CometBFT корень транзакций — `data_hash` заголовка CometBFT, мы его не
пересчитываем.

### 8.3 Связь с консенсусом
`state_root` после исполнения блока `H` возвращается в `FinalizeBlock` как `app_hash` и попадает в заголовок
блока `H+1` (семантика CometBFT). Нода, получившая другой `app_hash`, не сможет принять следующий блок и
остановится — расхождение state обнаруживается в пределах одного блока.

## 9. Исполнение блока

### 9.1 Вход

```
struct CommitVote {
    validator_address: [u8; 20]
    power:             u64
    flag:              u8      // 1 = Absent, 2 = Commit, 3 = Nil (значения BlockIDFlag CometBFT)
}

BlockContext {
    chain_id, height: u64, time_ms: u64, proposer_address: [u8;20],
    last_commit: vec<CommitVote>,   // голоса за блок H−1 (decided_last_commit / proposed_last_commit)
    misbehavior: vec<...>,
    txs: vec<bytes>
}
```

**Время.** `Timestamp {seconds: i64, nanos: i32}` CometBFT конвертируется так: требуется
`0 ≤ seconds ≤ 9 223 372 036 854 775` и `0 ≤ nanos ≤ 999 999 999`, иначе fail-stop;
`time_ms = seconds × 1000 + floor(nanos / 1 000 000)`.

**`last_commit` (ADR-0015).** Это голоса за блок `H−1`, включённые в блок `H`:
- на высоте `initial_height = 1` список **пуст** (блока 0 не существует);
- на высоте `H ≥ 2` содержит по одной записи на каждого валидатора набора высоты `H−1`; подписавшими считаются
  только записи с `flag = Commit`; `Nil` (голосовал против) и `Absent` наград не получают;
- адаптер ДОЛЖЕН проверить: на высоте 1 список пуст; `flag ∈ {1, 2, 3}`; каждый `validator_address` присутствует
  в `ValidatorInfo`, адреса не повторяются, `power` совпадает с `ValidatorInfo.power`. Нарушение — fail-stop (консенсус уже проверил подписи; несовпадение
  означает рассинхронизацию набора валидаторов между CometBFT и state).
- Состав `last_commit` выбирает proposer блока `H` (любые > 2/3 power плюс опоздавшие голоса, которые он успел
  получить), поэтому он детерминирован для всех нод, но не обязательно включает всех подписавших (threat-model.md, №15).

### 9.2 Порядок применения (нормативно)

1. **Проверка контекста**: `height == ChainState.height + 1`, `chain_id` совпадает,
   `time_ms ≥ ChainState.last_time_ms`, `last_commit` по §9.1. Нарушение — fail-stop.
2. **Evidence**: каждая запись `misbehavior` порождает событие `validator_misbehavior`; state не меняется
   (consensus.md §8).
3. **Транзакции** — строго в порядке блока, для каждой:
   1. stateless-валидация (§6.2), затем stateful-валидация (§6.3) — в overlay транзакции (§7.5);
   2. при ошибке — результат с кодом ошибки, overlay отбрасывается (ни комиссии, ни nonce);
   3. при успехе — эффекты payload (§9.3), распределение комиссии, `nonce += 1`; overlay сливается; код `OK`.
4. **Эмиссия** — §9.5.
5. **ChainState**: `height = H`, `last_time_ms = time_ms`; supply-поля уже обновлены шагами 3–4.
6. **state_root** — вычисляется по §8.1.

### 9.3 Эффекты операций
Все операции checked; ошибка → `ERR_OVERFLOW` и откат tx (§1.1).

**Нулевые суммы (r4, ADR-0021).** Зачисление `0` yip на аккаунт — no-op: несуществующий аккаунт **не создаётся**,
существующая запись не перезаписывается. Правило действует везде, где аккаунт пополняется: эффекты транзакций, доля
Treasury в комиссии (`fee − burn_part` может быть 0 при `fee_burn_bps = 10000`) и эмиссия §9.5 (`share_i`, `p`, `t`
могут быть 0). Аккаунт появляется в state только при зачислении `≥ 1` yip (или из genesis, где `balance ≥ 1`).

**Transfer** `{to, amount}`: `sender.balance −= amount + fee; to.balance += amount`.

**Burn** `{amount}`: `sender.balance −= amount + fee; total_burned += amount; total_supply −= amount`.

**ClaimAchievement** — attestations.md §4.3.

**Комиссия** (для всех типов, после эффектов payload):
```
burn_part = floor(fee × fee_burn_bps / 10000)
treasury.balance += fee − burn_part
total_burned     += burn_part
total_supply     −= burn_part
```

### 9.4 Результаты транзакций
Для каждой tx в блоке ровно один результат, в порядке блока: `{code: u32, data: пусто, gas_wanted: 0, gas_used: 0,
log, events}`. В CometBFT консенсусный хеш результатов (`last_results_hash`) включает детерминированные поля
`code`, `data`, `gas_wanted`, `gas_used` — поэтому коды результатов являются частью консенсуса, а `log`, `codespace` и
события — нет (подтверждено для CometBFT v0.40.0, `abci/types/types.go` `DeterministicExecTxResult`; r5). `log` не должен
содержать недетерминированных данных в любом случае.

### 9.5 Эмиссия (нормативно)

```
reward = min(block_reward, max_issuance − total_minted)
v = floor(reward × reward_validators_bps / 10000)
p = floor(reward × reward_pool_bps / 10000)
t = reward − v − p

S = записи last_commit с flag = Commit, по возрастанию validator_address (побайтно)
W = Σ ValidatorInfo.power по S              // checked; W ≤ 2^60 по §11
если S пуст:  t = t + v
иначе:
    для каждого i из S (в этом порядке):
        share_i = floor(v × power_i / W)
        reward_account_i.balance += share_i
    t = t + (v − Σ share_i)

rewards_pool.balance += p
treasury.balance     += t
total_minted += reward;  total_supply += reward
```
Все операции checked; ошибка — fail-stop (§1.1). Если `reward = 0`, шаг не меняет state.

## 10. Обработка некорректных блоков

| Уровень | Что проверяется | Реакция |
|---|---|---|
| Консенсус (CometBFT) | подписи предложений и голосов, структура блока, `app_hash`, `last_commit`, `block.max_bytes` | блок/голос отклоняется движком |
| `PrepareProposal` | proposer исполняет кандидатов последовательно (§9.2 шаг 3) над копией state и оставляет только tx с кодом `OK`, сохраняя их порядок; суммарный размер ≤ `max_tx_bytes` запроса ABCI, где каждая tx стоит `1 + len(varint(len)) + len` байт — так её считает CometBFT (`Txs.Validate`; r5) | в предложение попадают только валидные tx |
| `ProcessProposal` | последовательное исполнение всех tx блока над копией state; хотя бы одна tx с кодом ≠ `OK` | **REJECT** всего предложения |
| `FinalizeBlock` | блок уже решён > 2/3 power, отклонить нельзя | полное исполнение §9.2; невалидные tx пропускаются с кодом ошибки детерминированно |
| Внутренние ошибки | §1.1 (fail-stop), отказ хранилища, нарушение инвариантов §7.3 | остановка ноды, блок не коммитится |
| Dev-режим (M1) | §14 | блок отклоняется целиком, state не меняется |

Требования:
- `ProcessProposal` — детерминированная функция (state `H−1`, предложение). Он использует **тот же код исполнения**,
  что и `FinalizeBlock`, и не изменяет state: может вызываться несколько раз на одной высоте (разные раунды).
- `FinalizeBlock` ДОЛЖЕН исполнять блок заново (или использовать кеш по хешу блока): решён может быть блок, который
  нода не проверяла в `ProcessProposal`, а full nodes `ProcessProposal` не вызывают вовсе.
- Если `ProcessProposal` вернул ACCEPT для блока, `FinalizeBlock` того же блока даёт все коды `OK`.

## 11. Genesis (ADR-0016)

```
struct Genesis {
    chain_id:         string
    genesis_time_ms:  u64
    initial_height:   u64                   // ДОЛЖНО быть 1
    params:           ChainParams
    accounts:         vec<GenesisAccount>   // строго по возрастанию account_id
    validators:       vec<GenesisValidator> // строго по возрастанию consensus_address(consensus_pubkey)
    issuers:          vec<Issuer>           // строго по возрастанию issuer_id
    achievements:     vec<Achievement>      // строго по возрастанию (issuer_id, achievement_id)
}
struct GenesisAccount   { account_id: [u8;32], balance: u64 }
struct GenesisValidator { consensus_pubkey: [u8;32], power: u64, reward_account: [u8;32], name: string }

genesis_hash = H_tag("YAP/genesis/v1", YCS(Genesis))
```

### 11.1 Единый источник
Genesis YapChain хранится в поле `app_state` файла `genesis.json` CometBFT в JSON-представлении (§11.3).
`InitChain` получает его как `app_state_bytes`. Отдельного файла, который мог бы разойтись с CometBFT, нет.
В dev-режиме тот же JSON читается из файла.

### 11.2 Правила валидации
Нарушение любого правила → genesis невалиден, нода не стартует.
1. `chain_id` по §2; `initial_height == 1`.
2. `params` проходят правила §12.
3. `accounts`: строго возрастающие `account_id` (значит, без дублей); `balance ≥ 1`;
   `Σ balance` вычисляется checked и `≤ params.max_issuance`.
4. `validators`: 1..32 записей; строго возрастающие `consensus_address`; `consensus_pubkey` уникальны и проходят
   проверку §5.7; `power ≥ 1`; `Σ power ≤ 2^60` (лимит CometBFT `MaxTotalVotingPower = 2^63/8`); имена по §3.3.
5. `issuers`, `achievements`: правила attestations.md §3.2; каждый `achievement.issuer_id` существует.
6. JSON-представление разбирается строго (§11.3).

### 11.3 JSON-представление
- Суммы и все `u64` — **строки** в десятичной записи по регулярному выражению `^(0|[1-9][0-9]{0,19})$` с проверкой
  `≤ U64_MAX`; JSON-числа для них запрещены. Разбор не зависит от locale, не допускает пробелов, знаков, `+`, `-`.
- `u8/u16/u32` — JSON-числа без дробной части и экспоненты в диапазоне типа.
- Адреса — Bech32m (§5.3); ключи — hex нижнего регистра, ровно 64 символа.
- Неизвестные поля и повторяющиеся ключи объектов — ошибка.
- Хеш считается **только** по YCS; JSON на `genesis_hash` не влияет.
- **HRP адресов** в genesis определяется по `chain_id`: `"yap"` — если `chain_id` равен `chain_id` mainnet (пока OPEN,
  не определён), иначе `"tyap"`. Адрес с другим HRP — ошибка.
- **Схема (r4, ADR-0021).** Корень — объект ровно с полями ниже; порядок ключей не важен, порядок элементов массивов —
  нормативный (§11.2: строго по возрастанию, иначе ошибка). Типы: `"u64"` — десятичная строка; `u8/u16/u32` —
  JSON-число; `addr` — Bech32m; `hex32` — 64 hex-символа нижнего регистра; `str` — JSON-строка.

```
{
  "chain_id":        str,
  "genesis_time_ms": "u64",
  "initial_height":  "u64",
  "params": {
    "min_fee": "u64", "fee_burn_bps": u16, "block_reward": "u64",
    "reward_validators_bps": u16, "reward_pool_bps": u16, "reward_treasury_bps": u16,
    "max_issuance": "u64", "max_tx_bytes": u32, "max_memo_bytes": u32,
    "max_tx_ttl_blocks": "u64", "epoch_length_blocks": "u64",
    "max_attestation_validity_s": "u64", "attestation_clock_skew_s": "u64",
    "max_claims_per_subject_per_epoch": u16
  },
  "accounts":   [ { "address": addr, "balance": "u64" } ],
  "validators": [ { "consensus_pubkey": hex32, "power": "u64", "reward_account": addr, "name": str } ],
  "issuers":    [ { "issuer_id": u32, "name": str, "status": u8, "epoch_budget": "u64",
                    "keys": [ { "key_id": u8, "pubkey": hex32, "status": u8 } ] } ],
  "achievements": [ { "issuer_id": u32, "achievement_id": u32, "reward": "u64", "policy": u8,
                      "max_per_epoch": u16, "status": u8 } ]
}
```
  Каждое поле обязательно; массивы могут быть пустыми, если §11.2 это допускает. `name` и `chain_id` проходят те же
  правила содержимого, что и в YCS (§2, §3.3).

### 11.4 InitChain
1. `chain_id` запроса совпадает с `Genesis.chain_id`; `initial_height == 1`; время запроса в мс равно
   `genesis_time_ms` (конвертация §9.1).
2. Набор валидаторов запроса (pubkey, power) совпадает с `Genesis.validators` как множество; `yapd` возвращает
   пустой список `validators` (принимает набор CometBFT).
3. Начальный state: аккаунты, `ValidatorInfo`, `ChainParams`, эмитенты, достижения;
   `ChainState = {height: 0, last_time_ms: genesis_time_ms, total_minted = total_supply = Σ balance, total_burned: 0}`.
4. Ответ содержит `app_hash = state_root` начального state. Поле `app_hash` в genesis CometBFT оставляется пустым:
   в CometBFT v0.40.0 непустой `app_hash` ответа заменяет значение из genesis и становится `AppHash` первого блока
   (`consensus/replay.go`; r5).
Любое несовпадение — fail-stop.

## 12. Параметры

```
struct ChainParams {
    min_fee:                     u64   // yip
    fee_burn_bps:                u16
    block_reward:                u64   // yip
    reward_validators_bps:       u16
    reward_pool_bps:             u16
    reward_treasury_bps:         u16
    max_issuance:                u64   // yip, включая genesis
    max_tx_bytes:                u32
    max_memo_bytes:              u32
    max_tx_ttl_blocks:           u64
    epoch_length_blocks:         u64
    max_attestation_validity_s:  u64
    attestation_clock_skew_s:    u64
    max_claims_per_subject_per_epoch: u16
}
```

Правила валидности:
- `min_fee ≥ 1`;
- `fee_burn_bps ≤ 10000`;
- `reward_validators_bps + reward_pool_bps + reward_treasury_bps == 10000`;
- `max_issuance ≥ 1`;
- `256 ≤ max_tx_bytes ≤ 65536`; `max_memo_bytes ≤ 1024`;
- `max_tx_ttl_blocks ≥ 1`; `epoch_length_blocks ≥ 1`; `max_attestation_validity_s ≥ 1`;
- `max_claims_per_subject_per_epoch ≥ 1`.

Значения testnet — в [economics.md §8](economics.md#8-сводка-параметров-testnet).

## 13. Коды результатов

| Код | Имя |
|---|---|
| 0 | `OK` |
| 1 | `ERR_DECODE` |
| 2 | `ERR_TOO_LARGE` |
| 3 | `ERR_VERSION` |
| 4 | `ERR_CHAIN_ID` |
| 5 | `ERR_SIGNATURE` |
| 6 | `ERR_FEE` |
| 7 | `ERR_MEMO` |
| 8 | `ERR_EXPIRED` |
| 9 | `ERR_TTL_TOO_LONG` |
| 10 | `ERR_NONCE` |
| 11 | `ERR_INSUFFICIENT_FUNDS` |
| 12 | `ERR_PAYLOAD` |
| 13 | `ERR_OVERFLOW` |
| 14 | `ERR_UNSUPPORTED` — операция определена протоколом, но не включена в данной версии ноды (только до M3, §15) |
| 20 | `ERR_ATTESTATION` |
| 21 | `ERR_CLAIM_ISSUER` |
| 22 | `ERR_CLAIM_ACHIEVEMENT` |
| 23 | `ERR_CLAIM_DUPLICATE` |
| 24 | `ERR_CLAIM_LIMIT` |
| 25 | `ERR_CLAIM_BUDGET` |
| 26 | `ERR_CLAIM_POOL_EMPTY` |
| 27 | `ERR_CLAIM_TIME` |

Коды стабильны: новый код получает новый номер, старые не переиспользуются.

## 14. Dev-блоки (только M1)

В M1 нет консенсуса, но нужен проверяемый журнал блоков для тестов replay и восстановления.

```
struct DevBlockHeader {
    version:         u16        // = 1
    chain_id:        string
    height:          u64
    time_ms:         u64
    prev_block_hash: [u8; 32]   // для height 1 — genesis_hash
    tx_root:         [u8; 32]   // §8.2
    state_root:      [u8; 32]   // state ПОСЛЕ исполнения этого блока
    producer_pubkey: [u8; 32]
}
struct DevBlock {
    header:    DevBlockHeader
    txs:       vec<bytes>
    signature: [u8; 64]          // Ed25519(producer, "YAPCHAIN/DEVBLOCK/V1\0" || YCS(header))
}
block_hash = H_tag("YAP/devblock/v1", YCS(header))
```

Проверки при импорте: высота = предыдущая + 1; `prev_block_hash` совпадает; `time_ms ≥ genesis_time_ms` для высоты 1
и `time_ms > time_ms` предыдущего блока для остальных, и в любом случае `time_ms ≤ 2^63 − 1` (диапазон меток времени
§9.1; r5); не более 10 000 транзакций и не более 65 536 байт каждая (§3.3; r5); `tx_root` совпадает; подпись валидна и ключ совпадает с dev-ключом
из конфигурации; **все** tx исполняются с кодом `OK` (аналог `ProcessProposal`); `state_root` совпадает с вычисленным.
Любое нарушение — блок отклоняется целиком.

`last_commit` в dev-режиме повторяет семантику CometBFT: на высоте 1 пуст, на высоте `H ≥ 2` — все валидаторы
genesis с `flag = Commit`. `proposer_address` — адрес первого валидатора по порядку.
Dev-блоки не являются протоколом сети и не используются в testnet.

## 15. Поэтапное включение операций
До реализации M3 нода декодирует `ClaimAchievement` (формат нормативен), но возвращает `ERR_UNSUPPORTED` на шаге T7.
Testnet запускается только с полным набором операций, поэтому в живой сети этот код не встречается.
