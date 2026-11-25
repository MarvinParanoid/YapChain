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
