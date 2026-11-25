# Непрерывная интеграция

## Сборки и тесты (`.github/workflows/ci.yml`)
12 заданий build/test: {x86_64 `ubuntu-24.04`, aarch64 `ubuntu-24.04-arm`} × {GCC 14, Clang 18} ×
{Debug, Release, ASan+UBSan}; плюс воспроизводимость векторов и libFuzzer.

## GCC ASan на aarch64 (M2.6.1)
**Наблюдение** (run 45450447412, коммит `b0d8e5f`): на `ubuntu-24.04-arm` тесты внутри процесса с GCC 14.2 ASan+UBSan
в 60–110 раз медленнее, чем с Clang 18.1 ASan+UBSan на том же раннере (медиана 90×, 19 тестов ≥ 0,5 с):
`crash:long run` > 1800 с (таймаут) против 28 с, `fuzz_smoke` 1151 с против 12 с. Тесты, где время уходит в
неинструментированный код и порождение процессов (`cli:dev`), — лишь в 3 раза. На x86_64 GCC и Clang ASan
почти одинаковы (11 и 10 с; 5 и 4 с). Предупреждений рантайма ASan в логе нет.

**Причина не установлена.** Флаги одинаковы (`-fsanitize=address,undefined -fno-sanitize-recover=all
-fno-omit-frame-pointer`, Debug), `ASAN_OPTIONS` — из пресета (`detect_stack_use_after_return=1` и др.).
Гипотезы: стоимость `detect_stack_use_after_return` в GCC на aarch64; стоимость UBSan GCC; раскрутка стека при
каждом malloc/free. Локально проверить нельзя: нет aarch64-машины (qemu-user без aarch64-тулчейна, к тому же
эмуляция искажает время).

**Диагностика:** `asan-arm64-diagnose.yml` (ручной запуск `workflow_dispatch`): GCC и Clang на arm64, сборки
ASan+UBSan и только ASan (`-DYAP_SANITIZE_UBSAN=OFF`), варианты `ASAN_OPTIONS` (как в пресете; без
`detect_stack_use_after_return`; `malloc_context_size=0`; `fast_unwind_on_malloc=0`), одинаковая нагрузка
(`fuzz_smoke`, тест SMT), время, RSS, page faults, переключения контекста, профиль `perf` (если доступен).
Таблицы — в summary, файлы — в артефактах.

**Пока причина не найдена:** только в задании `aarch64/gcc/asan` долгие тесты идут в масштабе 25 %
(`-DYAP_LONG_TEST_SCALE_PERCENT=25`): `crash:long run` — 250 блоков вместо 1000 (те же проверки каждого блока,
те же точки сбоя, ≥ 3 перезапусков и ≥ 2 сбоев, отрезки пропорционально короче), `fuzz_smoke*` — 25 % итераций.
Все остальные 11 заданий и локальные прогоны выполняют их полностью. Исправленной проблема не считается, пока
это не подтвердит прогон на настоящем arm64.

## Живые тесты и TSan (M2.6.1)
Задания на x86_64 (`ubuntu-24.04`), в дополнение к 12 заданиям build/test, где живые тесты не регистрируются:

| Задание | Что делает |
|---|---|
| `live` | Go 1.27.4 (`actions/setup-go`, без кеша), CometBFT v0.40.0 из архива тега с проверкой SHA-256 и `go mod verify` (`localnet.py build-cometbft --download`, ADR-0027); GCC 14 Debug с `YAP_COMETBFT_BIN`; проверка, что зарегистрированы ровно 3 теста `integration:(live_cometbft|live_multival|live_netcli)` (пустой выбор — ошибка); их запуск |
| `tsan` | то же с Clang 18 и пресетом `tsan` (TSan-сборка `yapd` в живых тестах); весь набор, кроме долгих сценариев `fault`; только задокументированные подавления LMDB (`cmake/tsan-suppressions.txt`) |

Ограничения TSan на GitHub runners: LLVM 18 TSan не размещает теневую память при 32-битной рандомизации mmap новых
ядер («unexpected memory mapping»), поэтому задание ставит `vm.mmap_rnd_bits=28` (только на раннере). TSan
замедляет код в 5–15 раз: живые тесты чувствительны ко времени, при нестабильности их нужно разбирать, а не
отключать. Новых подавлений не добавлено.

## Долгие сценарии (`.github/workflows/extended.yml`)
Только ручной запуск (`workflow_dispatch`): сценарии отказов M2.5 (`-L fault`, 7 тестов: infra, quorum, partition,
latency, crash, crash_disk, fullnode) и soak (`-C soak`), x86_64, GCC 14 Debug, по 180 минут. Перед запуском
проверяется регистрация (7 и 1 тест). Логи и отчёт soak (без ключей) сохраняются как артефакты.
Предлагаемое расписание — раз в неделю (`cron: "0 3 * * 1"`); **не включено** без отдельного согласования.


## Закрепление и права
Все сторонние actions закреплены полными SHA коммитов (с тегом в комментарии): `actions/checkout` v7.0.1,
`actions/upload-artifact` v7.0.2, `actions/setup-go` v7.0.0 (используется заданиями живых тестов; самый новый релиз старше двух недель на
момент закрепления). Права workflow — `contents: read`. У каждого задания `timeout-minutes`; `concurrency` отменяет
устаревший прогон той же ветки или PR, кроме `main`, где каждый коммит сохраняет полный результат. Кеширования
нет. При падении сохраняются логи тестов (`Testing/Temporary`) и логи нод (без ключей и баз).
Локальная проверка: `python3 -I tools/ci/check_workflows.py` (тест CTest `ci:workflows`) — синтаксис, закрепление,
права, таймауты, отсутствие `continue-on-error`, раскрытие матрицы в 12 заданий.
