# Отчёт Task 1 — детерминированный каталог скиллов

## RED evidence

Добавлен Python-тест `CorsairsUE.Scripts.tests.test_build_skill_catalog` до production-скрипта. Первый запуск:

```text
python3 -B -m unittest CorsairsUE.Scripts.tests.test_build_skill_catalog -v
ModuleNotFoundError: No module named 'build_skill_catalog'
```

После добавления C++ automation-тестов контракт проверен focused Editor automation runner: `Corsairs.Skill.Catalog` — 3/3 Success, exit 0.

## Изменения

- Добавлен SQLite read-only exporter `CorsairsUE/Scripts/build_skill_catalog.py`: один ordered query, `mode=ro`, сортировка по `skillId`, UTF-8 + конечный LF, атомарная публикация через временный файл и `os.replace`.
- Добавлены Python-тесты deterministic bytes, literal fixtures (skill 1 entity, skill 4 ground), duplicate rejection и real census.
- Сгенерирован `CorsairsUE/Data/skills.json`: schemaVersion 1, 411 записей, уникальные отсортированные IDs.
- Добавлены `FCorsairsSkillDefinition`, `FCorsairsSkillCatalog` и строгий JSON loader без SQLite/fallback: schema version, unknown fields, типы, диапазоны, duplicate IDs и target mode проверяются fail-closed.
- `applyType` 1/3 классифицируется как Entity, 2 как Ground; `habitatMask` экспортируется из `tar_type`.
- `skills.json` зарегистрирован как UFS RuntimeDependency в `CorsairsGame.Build.cs`.

## Команды и результаты

```bash
python3 -B -m unittest CorsairsUE.Scripts.tests.test_build_skill_catalog -v
# Ran 3 tests ... OK

python3 -B CorsairsUE/Scripts/build_skill_catalog.py \
  "$PWD/databases/gamedata.sqlite" "$PWD/CorsairsUE/Data/skills.json"
# schemaVersion=1, 411 skills; output ends with LF

/Users/Shared/Epic\ Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh \
  CorsairsUEEditor Mac Development \
  -Project="$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
# Result: Succeeded; CorsairsSkillCatalog.cpp/tests.cpp compiled and linked

git diff --check
# без ошибок
```

Focused Editor automation: `Corsairs.Skill.Catalog` — 3/3 Success, exit 0.

## Self-review

- Runtime loader не открывает SQLite, не содержит fallback-каталога и публикует состояние только после полной валидации временной map.
- Python exporter использует ровно один `SELECT ... FROM skills ORDER BY id` и read-only URI.
- JSON fixture проверен на 411 записей, skill 1/4 и конечный LF.
- Изменения ограничены файлами Task 1; посторонний `docs/superpowers/plans/2026-08-12-original-mouse-control.md` не включается в commit.

## Commit

Будет создан один логический commit: `feat(ue): add authoritative skill catalog`.

## Concerns

- Полный runtime Automation запускал координатор; локально выполнен разрешённый incremental Editor build, GUI/сервер не запускались.
