# Дизайн parity эталонной зоны Garner и P0-движения

**Дата:** 2026-08-07
**Ветка:** `qwertyhq/codex-ue-parity`
**Эталонная позиция:** `garner (223325, 278475)`
**Радиус проверяемой зоны:** 8 000 см

## Цель и граница первого цикла

Первый цикл состоит из двух параллельных веток. Визуальная ветка
восстанавливает участок Garner, который игрок видит из эталонной позиции:
землю, типы объектов, их высоту и ориентацию. Исправления классификации и
трансформов применяются ко всему уровню, но строгая визуальная приёмка
относится к радиусу 80 м вокруг персонажа. Movement-ветка устраняет
подтверждённую рассинхронизацию после `BLOCK`/`FAILEDACTION`, чтобы игрок
мог пройти по свободному участку и корректно остановиться у препятствия.

Это измеримый этап общей цели, а не новое определение готовой игры. После
его приёмки параллельно запускаются расширение paged-пайплайна на остальной
Garner и полноценные pathfinding/click-to-move/`ATTR_MSPD`; затем
исправляются idle/run, map travel, эффекты и remaster-lighting.

## Подтверждённые причины текущего кадра

1. UE назначает одну доминирующую texture целому terrain-чанку 512×512
   клеток. Чанк `garner_terrain_04_05` получает `grass05`, хотя клетка
   персонажа `(2233,2784)` имеет базовый слой `brick05`. В окне 41×41 клетка
   80,7% оснований каменные; `grass05` занимает только 18,6%.
2. `.obj`-записи `type=1` являются эффектами, но проходят через
   `scene_objects`. Совпадающие ID превращают волны, дым и брызги в здания.
   На Garner это 3 026 effect-записей и 13 862 ложных mesh instances. В
   радиусе 61,03 м от эталонной точки находятся 143 ложных instances.
3. `yaw` делится на десять, хотя оригинал хранит градусы.
4. `heightOff` используется как абсолютная Z-координата. Оригинал добавляет
   его к поверхности terrain.
5. Scene models и characters сейчас используют один упрощённый height map,
   хотя оригинал применяет к ним два разных алгоритма.
6. Показанный пользователю кадр вошёл по серверной позиции
   `(224030,279600)`, а эталон снят в `(223325,278475)`: camera target был
   сдвинут на 1 328 см. Такой кадр полезен как доказательство дефектов, но не
   является корректным сравнением landmark-to-landmark.
7. Отдельный аудит подтверждает, что текущий WASD-path не воспроизводит
   server-authoritative движение оригинала: первые 0,5 с теряются, MOVE и
   FAILEDACTION не разбираются, pathfinding и reconciliation отсутствуют.
   Поэтому «ходить нельзя/движение не принимается» считается самостоятельным
   P0, а не визуальным побочным эффектом.
8. Последний runtime уточняет механизм P0: input, possession и локальный
   `CharacterMovement` работали — клиент отправил 58 двухточечных MOVE.
   После наблюдаемого packet ID 41 сервер начал непрерывно присылать
   `MSTATE_BLOCK`, а 16 запросов получили
   `FAILEDACTION(MOVE, EXISTACT)`. UE проигнорировал все эти ответы и после
   каждого socket-send продвигал локальный baseline, навсегда оторвав его от
   подтверждённой сервером точки. Из-за ранней перезаписи сервером общего
   `m_ulPacketID` эти сообщения нельзя один-к-одному приписывать client
   packets 41–58.

Material fallback уже устранён и не объясняет эти расхождения.

## Выбранный подход

Terrain восстанавливается детерминированным offline-bake. В отличие от
прежней схемы, UE не выбирает «доминирующий» слой. Baker повторяет
фиксированный DX9-порядок слоёв и создаёт небольшие потоковые страницы.

Страница покрывает 128×128 исходных клеток и имеет размер 4096×4096, то есть
32 texels на метр. При эталонной камере расстояние до target равно
61,032778 м, vertical FOV — 32°, а вертикальный охват target plane —
35,001735 м. Для кадра 1920×1080 это 30,8556 screen pixels на метр, поэтому
32 texels/м не теряют detail в центре эталонного кадра.

Пересечение literal camera frustum с плоскостью terrain имеет map bounds
`X=219354..227296`, `Y=275678..280220` см. Оно целиком находится в одной
странице `(17,21)` с bounds `X=217600..230399`,
`Y=268800..281599` см. Проверка исходной section table подтверждает, что в
этой странице нет absent sections. Радиус 80 м используется отдельно для
проверки окружающих scene objects и не заставляет bake-ить невидимую землю.

RGBA-страница не собирается целиком в памяти. Baker генерирует одну строку
`4096 × RGBA8` за раз и сразу передаёт её streaming-zlib writer:

- peak RSS baker не выше 128 MiB;
- один page asset не больше 4096×4096;
- итоговый PNG reference-страницы не больше 96 MiB;
- RGBA output buffer не больше одной строки, то есть 16 KiB;
- cache декодированных source textures не больше 32 MiB;
- одновременно загружены только секции `.map`, пересекающие текущую страницу
  и её одноклеточный halo;
- UE включает texture streaming для импортированных страниц.

Текущий `PngWriter` с несжатым deflate для этих страниц не используется:
`ImageCodec` получает потоковый zlib writer. Zlib становится явной CMake
dependency конвертера; отсутствие библиотеки останавливает конфигурацию.
Исходные PNG декодируются уже отслеживаемым
`sources/Libraries/stb_image/stb_image.h`.

Текущие `ReadWholeFile → ParseMap → MapTerrain::Tiles` не участвуют ни в
reference-bake, ни в воспроизводимой конверсии full-map rasters. Для Garner
один `Tiles` занял бы 240 MiB, чтение исходного `.map` — ещё 50,8 MiB, поэтому
сохранить эту архитектуру и одновременно заявлять gate 128 MiB нельзя.

Четырёхслойный runtime shader и UE Landscape в этот цикл не входят. Первый
создаёт лишний shader/performance-риск, второй требует повторной миграции
геометрии и коллизии.

## Архитектура

### 1. Секционный reader и потоковые writers

Новый `MapSectionReader` открывает seekable `.map`, читает только
`MapFileHeader` и offset table и валидирует каждое ненулевое смещение против
размера файла. `ReadSection(sectionX, sectionY)` делает один bounded seek/read
и возвращает ровно `sectionWidth × sectionHeight` тайлов либо явный
`absent`. В памяти нет массива `Width × Height`.

Для page-bake reader материализует только прямоугольник 128×128 клеток и
одноклеточный halo, необходимый интерполяции на правой и нижней границе.
Любой absent sample внутри owned 128×128 или required-present rect
фатален. Offset-zero section в right/bottom halo разрешена: оригинальный
`MPMap::GetTile` возвращает для неё shared default tile с
`fHeight=-2.0 m`, `dwColor=0xffffffff` и `dwTColor=0`. Поэтому
presence bit авторитетен: нулевые или stale bytes в absent `MapTile`
никогда не интерпретируются как реальный raw tile. Секции
освобождаются после завершения страницы. Таблица смещений, page tiles,
alpha atlas, одна output row и ограниченный texture cache входят в общий
gate RSS 128 MiB.

Полные `.height.r16`, `.block.raw` и `.region.raw` тоже строятся без
`MapTerrain::Tiles`: writer заранее задаёт конечный размер файла, заполняет
absent ranges нулями и пишет строки каждой присутствующей секции через
`seekp`. Таким образом clean rebuild остаётся воспроизводимым и для runtime
height samplers, но RAM не растёт пропорционально площади карты.

Существующий span-based `ParseMap` остаётся для малых unit fixtures и
совместимости. Production-orchestrator завершает работу ошибкой, если
reference/full-map команда попала в `ReadWholeFile`, создала
`MapTerrain::Tiles` размером карты либо превысила peak RSS.

### 2. Канонический terrain resolver

`AssetConverter` остаётся единственным владельцем бинарной семантики `.map`.
Из `MapTile` выделяется чистый resolver, возвращающий до четырёх пар
`(textureId, alphaMask)`.

Правила совпадают с оригиналом:

- `BaseTex=0` означает отсутствие land texture;
- базовый слой непрозрачен;
- верхние слои читаются по порядку до первого `textureId=0`;
- пары после первого нулевого ID являются неиспользуемым хвостом;
- верхний слой с `alphaMask=0` не меняет цвет;
- alpha 1..15 выбирает соответствующий literal rectangle угловой маски из
  `alpha/total.png`, а не scalar opacity; ID 0 является отдельным no-op и не
  семплирует atlas.

Один shared production resolver в Task 5 `TerrainPageBaker.h`
владеет corner color/height semantics для baker и mesh:

```cpp
struct LegacyTerrainCornerSample {
    std::array<std::uint8_t, 4> Diffuse;
    double HeightCm;
};

[[nodiscard]] LegacyTerrainCornerSample ResolveLegacyTerrainCornerSample(
    const MapTile& tile, bool present) noexcept;
```

При `present=true` он декодирует `Color` точными legacy shifts и
возвращает signed `Height*10 cm`. При `present=false` он возвращает
literal `{255,255,255,255}` и `-200 cm`, не читая ни одного поля
`tile`. Это не test-only seam: Task 5 обязан брать из него corner
diffuse, а Task 6 — height, не дублируя presence/default logic.

`MapSectionReader` передаёт per-section presence mask в page manifest. Она
нужна для отчёта и будущего sea pass. Owned и required-present
absent section фатальна; generic absent owned cell давала бы прозрачные
pixels, но reference-page `(17,21)` обязана иметь ноль absent sections.
Halo не входит в page mask, absent/unresolved counts и used texture IDs.
В частности, default tile texture 22 не семплируется и не добавляется
в `usedTextureIds` только из-за absent halo: для owned quad halo
поставляет только corner
diffuse/height, а texture layers и UV берутся из owned cell. Анимированное
море не подменяется плоской заглушкой и остаётся отдельным следующим циклом.

Каталог `textureId -> source path` читается из отслеживаемой
`databases/gamedata.sqlite`, таблица `terrains`. Terrain textures находятся в
`Client/texture/terrain`, alpha atlas — в
`Client/texture/terrain/alpha/total.png`.

### 3. Paged albedo baker

Для каждого output pixel baker выполняет исходный fixed-pipeline порядок:

1. определяет source cell и локальную координату;
2. семплирует base texture с периодом 4×4 клетки;
3. последовательно накладывает верхние слои по выбранным четвертям
   `total.png`;
4. интерполирует RGB565 tint по тем же двум треугольникам terrain quad;
5. вычисляет static vertex diffuse по исходной формуле;
6. умножает texture composite на vertex diffuse.

Все четыре corner samples читаются через
`ResolveLegacyTerrainCornerSample`. Для present tile RGB565 декодируется
по legacy shifts. Для absent halo corner нет
raw RGB565: берётся literal runtime `dwColor=0xffffffff`, то есть
diffuse `{255,255,255,255}`, и `dwTColor=0`. Подставить сюда raw
`0xffff` нельзя: legacy decode дал бы `{248,252,248,255}`. Texture и
alpha UV по-прежнему вычисляются из owned cell; absent halo bytes на них
не влияют.

Terrain рендерится до того, как `CGameScene::_Render` устанавливает
`m_dwEnvColor=0.6` для scene objects. `RenderStateMgr::BeginScene` ambient не
меняет, а `MPRender::Init` инициализирует его белым `0xFFFFFFFF`. Поэтому
эталонный Garner terrain вызывает `EnableNormalLight(0)` и
`MPTile::RenderTerrain` вычисляет для каждого канала:

```text
legacyDiffuse = saturate(1.0 * RGB565 + dwTColor)
finalTerrain = textureComposite * legacyDiffuse
```

Reference-bake использует `dwTColor=0`, поскольку это runtime-вклад
динамических point lights, которого нет в `.map`. Он не маскируется
неподтверждённым UE sun light. Порт dynamic terrain tint остаётся отдельной
задачей и не влияет на доказательство выбора `brick05`. Значение `0.6`
запрещено применять к terrain: оно относится к более поздней object-pass.

Каждая production-попытка сначала создаёт уникальный private run
`artifacts/maps/runs/<run-id>/` ровно с семью immutable продуктами:

```text
garner.height.r16
garner.block.raw
garner.region.raw
garner.terrain.json
garner.albedo_17_21.png
garner.terrain_17_21.gltf
garner.terrain_17_21.bin
```

Единственная publication boundary — маленький top manifest
`artifacts/maps/garner.reference-albedo.json`. Внутри run нет второго
manifest, а consumer не выбирает run по имени, времени или порядку каталога.
Top manifest публикуется последним и содержит:

- точные границы source cells, `pixelsPerCell=32`, ambient `(1.0,1.0,1.0)` и
  `dwTColor=0`;
- полный provenance: нормализованные пути и SHA-256 `garner.map`,
  `gamedata.sqlite`, `alpha/total.png`, client root, а также отсортированное
  отображение каждого реально использованного texture ID в разрешённый
  catalog path и SHA-256 texture-файла;
- отсортированный список использованных texture IDs, точную section-presence
  mask, нулевые absent/unresolved counters и все budget/geometry metrics;
- семь нормализованных путей `runs/<one-run-id>/<canonical-leaf>`, SHA-256 и
  размер каждого файла, а также checked-сумму этих семи размеров.

Перед публикацией orchestrator заново открывает и хеширует inputs и все семь
outputs; callback-provided hashes не считаются доказательством. Все семь
outputs сначала становятся durable: POSIX делает file fsync каждого файла и
fsync run directory, Windows делает `FlushFileBuffers` каждого файла и
same-directory write-through finalization через `MoveFileExW`; затем семь
файлов ещё раз независимо хешируются/измеряются. Unsupported Windows directory
fsync и administrator volume flush не требуются.

Top manifest и installer используют единый поведенческий `durable_fs` adapter.
POSIX: exclusive same-directory temp, flush+file fsync, same-device
`rename`/`os.replace`, parent-directory fsync. Windows: `CreateFileW(CREATE_NEW)`,
`FlushFileBuffers`, same-volume `MoveFileExW` с
`MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH`, без
`MOVEFILE_COPY_ALLOWED`/copy-delete fallback. Ошибка сохраняет точную adapter
operation, path и `errno`/`GetLastError`. Существующий manifest сначала
сохраняется с точными bytes/mode в durable same-directory backup. Journal
phases `SNAPSHOT -> PREPARED -> REPLACED -> COMMITTED` и startup recovery
гарантируют exact rollback до commit; persistent rollback failure является
`RECOVERY_REQUIRED`, сохраняет backup/journal и печатает точную
recovery-команду.

Runtime installer независимо валидирует top manifest и семь hashes, но
устанавливает только `garner.block.raw` и `garner.terrain.json` в
`CorsairsUE/Data/Heights`; tracked `garner.height.r16` и run-private region не
заменяются. Оба runtime-файла проходят одну recoverable transaction со
same-directory stages/backups, тем же platform `durable_fs`, durable phases
`SNAPSHOT -> PREPARED -> BLOCK_REPLACED -> PAIR_REPLACED -> COMMITTED` и
startup recovery, поэтому успешный возврат не может подтвердить mixed pair.

Два production-прогона над теми же входами обязаны создать разные run IDs,
но byte-for-byte одинаковые семь outputs и одинаковые hash/size tuples.
`peakRssBytes` остаётся фактической OS process-lifetime метрикой и в каждом
manifest независимо проверяется как `0 < value <= 128 MiB`. Детерминированная
проекция после этой проверки нормализует только run-ID сегмент путей и заменяет
только `peakRssBytes` на projection-only unsigned zero; все остальные DTO fields
обязаны совпасть. Actual manifests не обязаны быть byte-identical. Повторная
установка byte-identical пары — истинный no-op без replace и изменения
identity/mode/mtime.

### 4. Page mesh и UE material

Terrain mesh режется по тем же границам 128×128 клеток. UV0 каждой страницы
нормализован в `[0,1]`.
Все 129×129 source vertices читаются через тот же
`ResolveLegacyTerrainCornerSample`:
present `Height` означает `raw*10 cm`, absent right/bottom halo означает
literal runtime height `-200 cm`, а bytes absent `MapTile` игнорируются.
Absent owned sample остаётся fatal.

Геометрический `Step` выбирается для каждой страницы из `{4,2,1}`. Converter
сравнивает высоту исходной per-cell поверхности с треугольниками
прореженного mesh. Берётся самый большой Step, для которого одновременно:

- max absolute vertical error не выше 5 см;
- ошибка на общей границе соседних страниц равна 0;
- RMS vertical error не выше 2 см.

Если Step 4 или 2 не проходит, Step 1 воспроизводит исходные вершины без
прореживания.

UE importer создаёт:

```text
/Game/Terrain/Reference/Garner/T_Garner_17_21
/Game/Terrain/Reference/Garner/MI_Garner_17_21
```

Базовый `M_TerrainReference` использует masked blend, texture streaming и
unlit/emissive output. Emissive здесь воспроизводит не «сырой albedo», а уже
вычисленный `textureComposite * legacyDiffuse`. Для parity-capture
фиксируются manual exposure и neutral tone curve; auto exposure, bloom,
color grading, fog, clouds и motion blur выключены.

Material получает явные StaticMesh/Nanite usage flags и назначается как
StaticMesh asset, так и level component. Component override со старым
`MI_grass05` является ошибкой. Старые dominant scripts не могут молча
обработать reference-pages.

#### Канонический terrain-base в Unreal и packaged runtime

Terrain Task 8 начинается только после GREEN/review/commit terrain Tasks 1–7
и Movement Task 6. Единственный вход из Task 7 — атомарно опубликованный
`artifacts/maps/garner.reference-albedo.json`; consumer не выбирает run по
имени, времени или порядку каталога. Перед первой Unreal-мутацией tracked
orchestrator повторно запускает production `terrain-reference`, валидирует
один run с семью файлами и дважды запускает runtime installer. Вторая
установка обязана быть истинным no-op.

После Movement Task 6 runtime/editor module graph имеет ровно такую границу:

```text
CorsairsGame !-> CorsairsImport
CorsairsImport !-> CorsairsGame
```

`CorsairsImport` остаётся `Type=Editor`, а `UnrealEd` является его private
dependency. Editor tests загружают GameMode через `FSoftClassPath`, не
включают header из `CorsairsGame` и не создают обратное ребро. Сохранение
`CorsairsGame -> CorsairsImport` является ошибкой Mac Game build и cook, а
не допустимым способом получить Editor API в runtime.

Clean-checkout builder владеет только этими каноническими объектами:

```text
/Game/Maps/Garner
/Game/Maps/Garner.Garner
/Script/CorsairsGame.CorsairsGameMode
/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot
/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21
/Game/Terrain/Reference/Garner/SM_Garner_17_21
/Game/Terrain/Reference/Garner/T_Garner_17_21
/Game/Terrain/Reference/M_TerrainReference
/Game/Terrain/Reference/Garner/MI_Garner_17_21
```

Builder удаляет/пересоздаёт только ignored/generated package Garner, ставит
ровно один marker и exact native GameMode, сохраняет и повторно загружает
world. Marker-only world не является playable или visual acceptance.
Importer затем выполняется дважды; второй проход не создаёт, не обновляет,
не удаляет и не сохраняет ни одного объекта/package, а его пять финальных
package hashes byte-for-byte совпадают с первым проходом. Независимый checker
читает сохранённые packages и reports с диска и ничего не мутирует.

Texture/package readback обязан подтвердить весь sampling contract:

```text
Texture2D source = 4096x4096 RGBA8
source/import SHA-256 = manifest files.albedo.sha256
sRGB = true
compression = TC_DEFAULT
filter = TF_BILINEAR
address X/Y = TA_CLAMP
mip generation = TMGS_FROM_TEXTURE_GROUP
LOD group = TEXTUREGROUP_WORLD
LOD bias = 0
never_stream = false
TextureSample = SAMPLERTYPE_COLOR + SSM_FROM_TEXTURE_ASSET
parameter = BaseColorTexture
RGB -> MP_EMISSIVE_COLOR
A -> MP_OPACITY_MASK
material usage = MATUSAGE_STATIC_MESH + MATUSAGE_NANITE
mesh Nanite = enabled
mesh slot 0 = MI_Garner_17_21
level component slot 0 = MI_Garner_17_21
```

`CorsairsGame.Build.cs` stages как UFS runtime dependencies с сохранением
project-relative путей ровно эти production inputs:

```text
Data/character_map.json
Data/Heights/garner.block.raw
Data/Heights/garner.terrain.json
```

Task 7 installer выполняется до Game build и cook. Package audit повторно
хеширует все три staged/archive файла, требует Garner map/assets и запрещает
любой runtime binary/module descriptor `CorsairsImport`. Development-only
`Corsairs.Terrain.ReferenceRuntime.CookedWorld` запускается из packaged Game
под `-NullRHI`, через runtime `LoadObject<UWorld>` загружает exact
`/Game/Maps/Garner.Garner`, проверяет world/GameMode/actor/material bindings и
читает три файла через те же `FPaths::ProjectDir()/Data/...` пути. Он не
запускает BeginPlay, login или network.

Terrain orchestrator выполняет строго одну тяжёлую команду за раз в таком
порядке:

```text
Task 7 manifest + installer twice -> Editor build
    -> map builder -> import pass 1 -> import pass 2 -> read-only checker
    -> Editor automation -> Game build
    -> unique clean cook/stage/pak/archive with the just-built receipt
    -> packaged NullRHI runtime automation
```

Каждый subprocess получает отдельную process group, точные PID/PGID и
foreground wait. Все тяжёлые команды идут под `nice -n 10`, CMake/UE
parallelism ограничен двумя actions, перед каждым запуском проверяются
конкурирующие UE/client/build processes и `pmset -g therm`. Thermal warning
останавливает цепочку до запуска. После каждого шага и при любом
success/failure/timeout/interrupt orchestrator завершает и `wait`-ит только
собственную process tree; broad process-name kill запрещён.

До первой мутации orchestrator сохраняет exact bytes/existence/mode/hash
предыдущих top manifest, двух runtime-файлов, terrain-base bundle и пяти
managed package families со всеми `.uasset/.umap/.uexp/.ubulk/.uptnl`
sidecars. 0600 journal, same-volume recovery directory, file/parent fsync и
startup recovery обеспечивают полный rollback. Неуспешный rollback сохраняет
recovery evidence и возвращает `RECOVERY_REQUIRED`; частичный success
запрещён. Только после всех gates атомарно публикуется
`artifacts/maps/reports/garner-terrain-base.json` с нормализованными путями и
повторно вычисленными SHA-256 для Task 7 manifest/seven files, runtime pair,
reports, пяти packages, Editor/Game build identities, cook/package,
executable и packaged runtime report.

`garner-terrain-base.json` является immutable input для scene parity Task 8,
но не заменяет его более строгий `garner-base-bundle.json`. Только scene Task
8 населяет полный город и владеет original/UE capture 1920x1080 на tick 120,
side-by-side, landmark projection и восстановлением DB/`system.ini`. Terrain
Task 8 не запускает ни один графический клиент и не заявляет visual parity по
marker-only world.

### 5. Типы объектов и два height sampler

Scene placement применяет одну политику:

- `type=0` разрешается через `scene_objects`;
- `type=1` считается как `deferred effect`, но никогда не проходит через
  scene-model lookup;
- неизвестный type останавливает сборку.

Manifest сохраняет для каждой записи стабильный source key:
`(sectionIndex, slotIndex, byteOffset)`. По нему checker связывает исходную
запись со всеми частями `.lmo` и HISM instances без эвристики по близости.
Поле `sScale` в `.obj` не применяется: оригинальный
`CGameApp::LoadMapSection` читает его, но для scene/effect objects вызывает
только `setHeightOff`, `setPos` и `setYaw`; встречающиеся там произвольные
значения не должны менять размер UE mesh.

Строго проверяемое множество объектов задаётся не ручным списком:

```text
ReferenceObjectSet =
  все записи type=0,
  чья source anchor-position находится не дальше 8 000 см
  от (223325,278475)
```

Измеренный Garner baseline этого множества — 1 634 записи: 1 625 anchors
стоят на `MapTile.Island=1`, 6 — на `Island=2`, ещё 3 попадают в отсутствующую
terrain section и потому в оригинале получают default tile с `Island=0`.
Эти числа входят в manifest и защищают границу множества от случайного
изменения.

Для каждой записи этого множества model должен разрешиться, а число
импортированных частей должно совпасть с catalog. Неразрешённый type-0 вне
этого множества остаётся отдельным warning и не ослабляет проверки внутри
зоны.

Placement manifest также хранит presence исходной terrain section и
`MapTile.Island` под каждым anchor. Для существующего area ID object material
выбирает его `env_color`, `light_color` и `light_dir`. Это per-object
свойство, а не один глобальный свет Garner. Для `Island=0`, которому нет
записи в `areas`, оригинальный renderer сохраняет предыдущий lighting state;
поэтому такой объект нельзя без доказательства подменять настройками area 1.
Если projected bounds любого из трёх объектов на absent section пересекают
world viewport эталонного кадра, capture прерывается до реализации
оригинального stateful draw-order. Если не пересекают, их геометрия и
transform всё равно проходят строгие placement gates, а lighting помечается
как `legacy-inherited-outside-capture`.

Island не является единственным входом object lighting. Из
`scene_objects` в manifest для каждого model ID обязательно переносятся
`type`, `enable_env_light`, `enable_point_light`, `shade_flag`, `size_flag`,
`point_color`, `env_color`, `range`, `attenuation` и `anim_ctrl_id`.
Renderer повторяет исходный порядок:

```text
lightingEnabled = enable_env_light | enable_point_light | shade_flag
directional area light enabled только при enable_env_light
point lights 1/2 enabled только при enable_point_light
shadeAmbient = tileRGB565 * (size_flag ? areaEnv : white) / 255
```

При всех трёх флагах, равных нулю, используется legacy-unlit texture pass:
area direction и ambient к объекту не применяются. При `shade_flag=0`
tile RGB565 не подмешивается.

Для `enable_point_light=1` resolver воспроизводит
`BeginUpdateSceneObjLight`: среди scene records с catalog `type=3` выбирает
две ближайшие к anchor без выдуманного distance cutoff, переносит source
range/attenuation/color и, если задан `anim_ctrl_id`, вычисляет keyframe из
таблицы `animated_lights` на capture tick 120 при фиксированных 30 Hz.
Point-light source, выбранные IDs, квадрат расстояния и вычисленные параметры
сохраняются в manifest. Synthetic test защищает исходный порядок выбора двух
ближайших и интерполяцию animated keyframes.

#### TerrainSurfaceHeight для scene models

Source coordinates переводятся в метры (`x/100`, `y/100`). Sampler берёт
`MapTile.Height` четырёх соседних клеток, переводит raw unit в 0,1 м и
интерполирует по треугольникам:

```text
v0 (0,0), v1 (1,0), v2 (0,1)
v2 (0,1), v1 (1,0), v3 (1,1)
```

Результат ниже `SEA_LEVEL=0` зажимается к нулю. Вне сетки используется
default sea height 0. Scene model получает:

```text
UE.X = source.X
UE.Y = -source.Y
UE.Z = TerrainSurfaceHeight(source.X, source.Y) + heightOff
```

#### CharacterGridHeight для characters

Character ground использует half-meter grid:

```text
gridX = trunc(source.X / 50)
gridY = trunc(source.Y / 50)
```

Значение берётся из соответствующей четверти `.block.raw`:

```text
magnitudeCm = (byte & 63) * 5
heightCm = (byte & 64) ? -magnitudeCm : magnitudeCm
```

Bit 128 является block flag и на высоту не влияет. Интерполяции нет.
Вне сетки высота равна 0. Character actor center получает
`CharacterGridHeight + capsuleHalfHeight`; local и remote characters
используют один этот алгоритм.

`.terrain.json` задаёт `gridWidth/gridHeight`; `.height.r16` хранит surface
height, `.block.raw` — character grid height и block flags. Квадратность
карты нигде не предполагается.

### 6. Полный basis для scene-model

Обычный `GltfWriter` переводит source vertex
`(x,y,z) -> (x,z,y)`, а UE glTF parser выполняет обратную перестановку
`(x,y,z) -> (x,z,y)`. Поэтому текущий imported local vertex снова равен
`(x,y,z)`. Одновременно world map отражает Y:

```text
Fmap: (x,y,z) -> (x,-y,z)
```

Если оставить asset-local basis неизменным, точный instance transform равен
`Fmap * Rsource` и имеет отрицательный determinant. Один UE yaw не может его
представить, а отрицательный HISM scale создаёт проблемы с culling, normals и
Nanite. Поэтому scene models получают отдельный converter profile
`SceneMap`; characters, equipment и terrain этот профиль не используют.

`SceneMap` запекает дополнительный local mirror во весь scene asset. После
обычного source→glTF преобразования применяется
`Ggltf=diag(1,1,-1)`, то есть source vertex в glTF становится
`(x,z,-y)`, а после UE import — `(x,-y,z) = Fmap(sourceVertex)`.
Операция применяется ко всему asset graph:

- positions и normals получают `Ggltf`;
- существующая смена winding выполняется второй раз, поэтому итоговый
  scene-profile сохраняет source index order;
- при появлении tangents их xyz получают `Ggltf`, а handedness `w`
  инвертируется;
- каждый уже resolved `.lmo` `MatModel` и dummy transform получает
  conjugation `Ggltf * M * Ggltf` ровно один раз;
- LOD и collision geometry зеркалятся тем же профилем; skinned scene asset
  допускается только после полного зеркала bones, bind/inverse-bind и
  animations, половинчатая конверсия является fatal error;
- scale HISM instance остаётся положительным `(1,1,1)`.

Иначе отдельные части многообъектного здания зеркалились бы вокруг
собственных origin и снова наслаивались. Scene-profile импортируется в
отдельный namespace `/Game/SceneParity`; один и тот же raw `.lgo` не может
молча использоваться одновременно как mirrored scene asset и как character
equipment.

Оригинал строит row-vector matrix через
`lwMatrix44RotateZ(-sourceYaw + PI)`. После local mirror actor rotation равен
conjugation `Fmap * Rsource * Fmap`; в соглашениях `FRotator` это:

```text
UEYaw = normalize(180° - sourceYawDegrees)
```

Literal golden expectations не вычисляются production helper:

| Source record | Source yaw | Expected UE yaw | Expected UE forward |
|---|---:|---:|---|
| Road Notice 03 ID 338 `(218400,270800)` | 0° | 180° | `(−1,0,0)` |
| Bell Tower ID 22 `(223200,277320)` | 90° | 90° | `(0,1,0)` |
| Bench 01 ID 314 `(223880,278550)` | −180° | 0° | `(1,0,0)` |
| Argent City Fence ID 37 `(217490,270700)` | 270° | −90° | `(0,−1,0)` |

Отдельный asymmetric golden не ограничивается forward vector. Source
triangle:

```text
A0=(0,0,0), B0=(2,0,0), C0=(0,1,0)
normal=(0,0,1), indices=(0,1,2)
resolved MatModel translation=(3,4,0)
map position=(1000,2000,300) см, source yaw=90°
```

После scene mirror и generic writer literal glTF node translation обязана
быть `(3,0,-4)`, normal — `(0,1,0)`, indices — `(0,1,2)`. После headless UE
import local points в сантиметрах обязаны быть
`A=(300,-400,0)`, `B=(500,-400,0)`, `C=(300,-500,0)`, а HISM с
`location=(1000,-2000,300)`, `yaw=90°`, positive scale должен дать:

```text
A_world=(1400,-1700,300)
B_world=(1400,-1500,300)
C_world=(1500,-1700,300)
```

Imported normal остаётся `+Z`, а one-sided triangle виден сверху. Все
expected записаны константами с допуском 0,01 см и не вычисляются
converter/helper-кодом.

### 7. Reducer server-authoritative movement

Input wiring не переделывается: runtime уже доказал, что legacy axes,
`EnhancedPlayerInput`, possession и `AddMovementInput` работают. Источником
ошибки является сетевое состояние после отправки.

`UCorsairsSession` становится единственным владельцем movement authority и
хранит:

```text
ConfirmedPosition  — последний endpoint из сообщения сервера;
PredictedPosition  — текущее положение локальной capsule;
ActiveBeginAction  — packetId, actionType и phase либо none;
PendingMove        — packetId, start, requested endpoint либо none;
ServerDrivenMove   — packetId и authoritative waypoints либо none;
QueuedEndpoint     — последняя желаемая точка, накопленная во время pending.
```

`ConfirmedPosition` инициализируется позицией `ENTERMAP` до
`AttachSession`; первый сегмент больше не отбрасывается. Одновременно
разрешён ровно один `PendingMove`, потому что `MC_FAILEDACTION` не содержит
packet ID и может быть безопасно сопоставлен только с единственным
незавершённым MOVE. Пока MOVE активен, новые позиции только заменяют
`QueuedEndpoint` и не отправляются серверу.

Session-level arbiter резервирует `ActiveBeginAction` **до** вызова
`Connection->Send` для любого `CM_BEGINACTION` (`MOVE`, `SKILL`,
item/equip/pickup). При ошибке socket-send reservation атомарно снимается; при
успехе остаётся до protocol-terminal. Пока reservation существует, любой
следующий BeginAction получает локальный `Busy`, не попадает в socket и не
ставится в неявную очередь. Это закрывает также окно между outbound
`SKILL+path` и первым server MOVE: сервер не может получить manual MOVE и
переписать общий `m_ulPacketID` раньше ответа skill. Команды, не являющиеся
`CM_BEGINACTION`, этим gate не блокируются.

Lifecycle reservation зафиксирован для всех уже существующих public calls:

- manual `MOVE`: terminal MOVE либо `FAILEDACTION(MOVE, ...)`;
- `SKILL`: `FAILEDACTION(SKILL, ...)`,
  `FAILEDACTION(MOVE, ...)` его path-phase либо terminal local
  `SKILL_SRC` с `state != FSTATE_ON`; terminal `MOVE/INRANGE` закрывает только
  `ServerDrivenMove`, но не сам skill reservation, потому что сервер после
  него запускает fight phase; любой другой terminal MOVE закрывает и
  `ServerDrivenMove`, и skill reservation, поскольку `CAction::DoNext`
  прерывает skill;
- `ITEM_USE`: matching `KITBAG`, `LOOK` или `ITEM_FAILED`;
- `ITEM_PICK`: matching `KITBAG` или `ITEM_FAILED`.

Success-notification должен иметь `worldId` локального actor и packet ID
reservation. Неизвестная комбинация остаётся `Busy` и поднимает protocol
error вместо timeout-разблокировки. Disconnect/Logout явно очищает все
reservations.

`CMD_MC_NOTIACTION` с `ActionType::MOVE` разбирает `ActionMoveData`.
`waypoints` обязан иметь ненулевое число целых пар `int32`; malformed payload
не меняет состояние и даёт protocol error. Последняя waypoint становится
server endpoint для local либо remote actor:

- `MSTATE_ON=0` обновляет подтверждённый путь, но не завершает pending;
- terminal bits `ARRIVE`, `BLOCK`, `CANCEL`, `INRANGE`, `NOTARGET` и
  `CANTMOVE` обновляют `ConfirmedPosition` и завершают pending;
- после `ARRIVE` можно отправить накопленный `QueuedEndpoint`, если он дальше
  порога;
- после отрицательного terminal state (`BLOCK`, `CANCEL`, `NOTARGET`,
  `CANTMOVE`) очередь очищается, локальная скорость обнуляется, capsule
  reconcile-ится к server endpoint и устанавливается rejection latch;
- для remote actor server waypoints являются единственным источником
  движения и facing; actor интерполирует путь, а terminal endpoint
  устанавливается точно.

Для local actor `McCharacterActionMessage.packetId` сопоставляется с
`PendingMove.packetId`. Session-level запрет всех параллельных
`CM_BEGINACTION` гарантирует, что сервер не перепишет этот ID до terminal.
Пока `PendingMove` существует, только совпавший ID может обновить
`ConfirmedPosition` и завершить текущий pending. Несовпавшее local MOVE в
этом состоянии считается stale/protocol error, не меняет position и не
создаёт reconcile-event.

При `PendingMove==none` local MOVE не считается автоматически stale: сервер
легитимно создаёт movement phase для `SKILL+path`, сохраняя packet ID
исходного skill. Такой packet открывает `ServerDrivenMove`, обновляет
authoritative path/`ConfirmedPosition` и reconcile-ит capsule по тем же
правилам. `MSTATE_ON` удерживает `ServerDrivenMove`, terminal закрывает его.
Его packet ID обязан совпасть с `ActiveBeginAction(SKILL)`.
Дубликаты фильтруются тем же `LastCompletedMove`; terminal уже завершённого
manual MOVE не может повторно открыть server-driven state.

Для skill-driven path значение terminal принципиально: только
`MSTATE_INRANGE` переводит reservation в fight phase. `ARRIVE`, `BLOCK`,
`CANCEL`, `NOTARGET` или `CANTMOVE` означают interrupt, закрывают reservation,
точно reconcile-ят endpoint и включают neutral/re-press latch; ожидать после
них `SKILL_SRC` запрещено.

Предварительный `MSTATE_ON` необязателен: direct `ARRIVE` или `BLOCK` для
текущего packet обрабатывается как полноценный terminal. Reducer хранит
`LastCompletedMove=(packetId,state,endpoint)`; идентичный повтор уведомления
идемпотентно игнорируется. Terminal старого packet после создания нового
pending также игнорируется и не откатывает confirmed state. Любой принятый
terminal, включая `ARRIVE`, точно reconcile-ит local capsule к последней
server waypoint; накопленный следующий path начинается строго с этого нового
`ConfirmedPosition`.

`CMD_MC_FAILEDACTION` разбирается как `McFailedActionMessage`. Packet ID в нём
нет, поэтому единственная `ActiveBeginAction` является обязательной
корреляцией. Для manual MOVE `actionType=MOVE` завершает pending, очищает
queued endpoint, останавливает prediction и возвращает capsule к
`ConfirmedPosition`. Для active SKILL как `actionType=SKILL`, так и
`actionType=MOVE` path-phase снимают skill reservation и закрывают
`ServerDrivenMove`. Failure другого типа не освобождает reservation.
`EXISTACT` не вызывает немедленный resend. Socket send означает только
доставку запроса и никогда не продвигает confirmed baseline.

С момента reservation `SKILL` и до его terminal pawn включает
authority-lock: удержанные legacy axes не вызывают `AddMovementInput`, не
сдвигают predicted capsule и не создают manual MOVE. Положение меняют только
server waypoints. После завершения skill lock переходит в тот же
neutral/re-press latch, поэтому удержанная всё время `W` не запускает manual
движение без release.

`BLOCK`, другой отрицательный terminal и `FAILEDACTION(MOVE, ...)` включают
rejection latch на стороне pawn. Ненулевые legacy axis при всё ещё удержанной
клавише не двигают capsule и не создают новый queued endpoint. Latch снимается
только после кадра, в котором обе оси `MoveForward` и `MoveRight` равны нулю;
следующий переход zero→nonzero считается новым intent и снова разрешает
prediction/MOVE. Поэтому «новый input» означает именно release/neutral с
последующим новым нажатием, а не очередной axis callback удержанной клавиши.

Обновление capsule передаётся наружу типизированным событием reducer:
`AcceptedPath`, `Terminal`, `Rejected`; `ACorsairsPlayerCharacter` применяет
его на game thread. Reconcile для terminal/rejected в этом P0 выполняется
точным teleport XY с последующим `CharacterGridHeight + capsuleHalfHeight`,
чтобы checker мог доказать отсутствие накопленного расхождения. Сглаживание
можно добавить после корректности.

Remote spawn использует тот же helper центра capsule. Текущий код ставит
центр ровно на `HeightAt` и из-за collision уже терял actors при
`SpawnActor`; новое правило:

```text
center.Z = CharacterGridHeight(X,Y) + scaledCapsuleHalfHeight
```

Полный `CFindPath`, server block/region raster, click-to-move и скорость из
`ATTR_MSPD` остаются следующим movement-циклом. Однако P0 считается принят
только если на свободном участке движение действительно подтверждается
сервером, а у известного препятствия клиент останавливается без packet flood
и постоянного рассинхрона.

## Воспроизводимая сборка и ошибки

Воспроизводимость имеет две вложенные, но не взаимозаменяемые publication
границы. `scripts/build_garner_reference_terrain.py` атомарно публикует
проверенный `garner-terrain-base.json` после Editor/Game/cook/package/NullRHI
цепочки выше. Scene parity Task 8 принимает только этот bundle по hash-link,
строит полный Garner и публикует собственный `garner-base-bundle.json` вместе
с dual-client capture. Loose reports, ignored `Content` и ручное состояние
Editor не являются входом ни одной границы.

Сквозной scene orchestrator:

1. собирает и тестирует `AssetConverter`;
2. потоково конвертирует `garner.map` и `garner.obj`;
3. создаёт reference-pages и проверяет budgets/hashes;
4. импортирует pages в headless UE;
5. пересоздаёт Garner без дублирования actors;
6. применяет фильтр type, исправленные rotation и height;
7. назначает reference material страницам в camera footprint;
8. запускает unit, automation и runtime checkers;
9. запускает movement reducer/possession/grounding tests;
10. готовит offline reference-character fixture;
11. последовательно запускает original и UE, сохраняя два 1920×1080
    parity-capture и общий manifest сравнения;
12. восстанавливает исходную строку reference-character даже при ошибке
    capture.

Сборка прерывается до сохранения нового уровня, если:

- реально используемый texture ID отсутствует в catalog;
- source texture или alpha atlas не читается;
- production path прочитал `.map` целиком или создал full-map `Tiles`;
- output нарушает memory/disk/page budget;
- height/layer array не совпадает с metadata;
- page `(17,21)` или camera-frustum bounds содержат absent section;
- exact Garner package/world/GameMode/marker/reference actor не совпадает;
- второй import-pass мутирует или сохраняет package;
- Game target содержит dependency или packaged module `CorsairsImport`;
- staged runtime data отсутствует или не совпадает по SHA-256;
- cook/package/packaged NullRHI runtime report не подтверждает Garner;
- rollback не восстановил точные bytes/existence/mode managed inputs;
- перед тяжёлым запуском обнаружен thermal warning или competing process;
- object type неизвестен;
- type-0 model не разрешён внутри радиуса 8 000 см;
- число scene parts для source key не совпало с catalog;
- остался dominant-material component override.

Неразрешённый type-0 за пределами reference-зоны остаётся warning с ID,
координатами и счётчиком. Это не считается доказательством полной Garner:
расширение зоны потребует устранить такой warning до приёмки новой области.

`Content` и `artifacts` остаются generated/ignored. Все действия их
восстановления задаются tracked source, scripts и manifests; ручное состояние
Editor не является входом.

## Проверки

### Converter tests

- данные после первого `textureId=0` не участвуют;
- `(2233,2784)` разрешается только в `brick05`, хвостовой `sand02`
  игнорируется;
- alpha ID 0 является no-op, IDs 1..15 выбирают literal atlas rectangles;
- texture UV повторяется каждые четыре клетки;
- RGB565 и ambient 1,0 дают literal RGBA8 values;
- synthetic 2×2 patch имеет заранее зафиксированный pixel hash;
- повторный bake даёт те же SHA-256;
- synthetic large-map fixture доказывает, что production reader не вызывает
  `ReadWholeFile` и держит не больше page+halo;
- Garner page+halo имеет ровно одну absent section-mask ячейку
  с local index `254` и ровно восемь absent tile samples
  `local x=128, y=112..119`;
- для правой Garner boundary высоты в cm точно
  `H(127,112..119)=-60`, `H(128,112..119)=-200`,
  `H(127,120)=H(128,120)=-100`; поэтому corner arrays
  `TL,TR,BL,BR` равны `[-60,-200,-60,-200]` для
  `y=112..118` и `[-60,-200,-100,-100]` для `y=119`;
- pure resolver test подаёт два радикально разных `MapTile` и
  controlled mutations всех полей при `present=false`: все вызовы
  дают literal white/-200 cm; cloned absent slots не меняют
  glTF/bin, а missing owned sample делает bake/mesh невалидным;
- reference output укладывается в 128 MiB RSS и 96 MiB file budget;
- max/RMS geometry errors проходят пороги 5/2 см.

### Height и placement tests

- non-coplanar surface с corner heights `0/100/200/0 см` даёт 75 см и в
  `(0.25,0.25)`, и в `(0.75,0.75)`; bilinear 56,25 см является ошибкой;
- surface ниже sea и out-of-range возвращают 0;
- block bytes `0x05`, `0x45`, `0x85` дают соответственно
  `25`, `−25`, `25 см`;
- одинаковый `modelId=1` с type 0 и 1 создаёт mesh только для type 0;
- catalog golden фиксирует, что Bell Tower ID 22 имеет lighting flags
  `0/0/0`, а Bench 01 ID 314 и Garden 01 ID 323 — `1/0/0`;
- Bell Tower использует legacy-unlit pass, Bench/Garden получают area 1
  directional/ambient; `shade_flag=1` synthetic model смешивает literal
  tile RGB565 с area env при `size_flag=1` и с white при `size_flag=0`;
- synthetic point-light set выбирает те же две ближайшие записи и тот же
  animated-light keyframe, что literal legacy evaluator;
- четыре literal yaw golden records дают значения из таблицы;
- asymmetric markers получают literal world positions из раздела 6;
- multi-part fixture зеркалит также `MatModel` и dummy offsets;
- synthetic triangle после UE import остаётся front-facing с normal `+Z`;
- при surface height 60 см и `heightOff=0` scene object получает Z=60 см;
- local и remote character имеют одинаковую нижнюю границу capsule/обуви.

### UE и runtime acceptance

Обязательные машинные gates:

- `AssetConverterTests` — PASS;
- Python unit suite — PASS;
- Mac Editor Development build — PASS;
- exact `/Game/Maps/Garner.Garner` с
  `/Script/CorsairsGame.CorsairsGameMode` — PASS;
- два import-pass дают zero-mutation/hash idempotence — PASS;
- Unreal Editor automation — PASS;
- Mac Game Development build без обеих зависимостей между
  `CorsairsGame`/`CorsairsImport` — PASS;
- unique Garner cook/stage/pak/archive с тремя UFS runtime-файлами и без
  `CorsairsImport` — PASS;
- packaged `Corsairs.Terrain.ReferenceRuntime.CookedWorld` под NullRHI — PASS;
- `garner-terrain-base.json` опубликован последним и независимо rehash-ится —
  PASS;
- material fallback/usage/translucent-Nanite counts — `0/0/0`;
- type-1 instances в scene mesh — 0;
- 143 ранее ложных instances в радиусе 61,03 м отсутствуют;
- для каждого source key из `ReferenceObjectSet` число parts совпадает с
  catalog, XY error anchor не выше 1 см, Z error не выше 1 см;
- cardinal forward-vector dot product не ниже 0,999;
- повторная сборка не меняет instance counts и page hashes.

Movement gates:

- possessed pawn с текущими Enhanced classes и legacy axis получает
  synthetic `MoveForward`, за ticks проходит больше 100 см; этот
  characterization test проходит до production-правки и защищает от
  ненужной замены input stack;
- `AttachSession` получает server spawn как initial confirmed position, а
  движение в первые 0,5 с создаёт первый path именно от неё;
- `MOVE ON -> ARRIVE` обновляет endpoint, очищает pending и отправляет не
  больше одного накопленного следующего пути;
- direct `Pending -> ARRIVE` и `Pending -> BLOCK` без предварительного `ON`
  корректно завершают action;
- два идентичных terminal notification дают один state transition, один
  reconcile и не отправляют queue повторно;
- stale terminal старого packet после создания нового pending не меняет
  confirmed position и не завершает новый pending;
- outbound `SKILL(packet S)` создаёт reservation до socket-send; попытка
  manual `MOVE(packet M)` до первого server reply возвращает `Busy` и не
  попадает в socket;
- `SKILL S -> MOVE ON/INRANGE S -> terminal SKILL_SRC S` открывает и
  завершает server-driven movement, подтверждает последнюю waypoint и лишь
  затем освобождает action reservation; удержанная ось всё это время не
  двигает capsule, а следующий manual MOVE после neutral/re-press начинается
  от confirmed endpoint;
- `SKILL S -> MOVE terminal S`, где state не содержит `INRANGE`, закрывает
  server-driven movement и skill reservation без ожидания `SKILL_SRC`,
  reconcile-ит endpoint и включает neutral latch;
- `SKILL S -> FAILEDACTION(MOVE, MOVEPATH/ACTFORBID)` и
  `SKILL S -> FAILEDACTION(SKILL, ...)` обе снимают reservation без изменения
  confirmed endpoint;
- `ITEM_USE/ITEM_PICK` reservation освобождаются только literal success/fail
  notification из lifecycle table; второй BeginAction до него остаётся
  `Busy`;
- `MOVE ON -> BLOCK` возвращает local capsule в последний server endpoint,
  обнуляет velocity/queue, включает rejection latch и не создаёт повторный
  packet при удержанной оси;
- `FAILEDACTION(MOVE, EXISTACT)` очищает pending и не продвигает confirmed
  position;
- `MOVE ON ->` локально отклонённый non-MOVE `BeginAction -> MOVE terminal`
  не отправляет второй action серверу, сохраняет packet ID и завершает
  pending;
- после `BLOCK` удержанная ненулевая ось в течение двух секунд даёт ноль
  новых MOVE; release/neutral и новое нажатие дают ровно один MOVE;
- malformed waypoint blob отклоняется без изменения reducer state;
- remote `MOVE` проходит все server waypoints, получает terminal endpoint и
  facing;
- local и remote spawn center равен
  `CharacterGridHeight + scaledCapsuleHalfHeight`, collision не уничтожает
  actor;
- live Garner выполняет два изолированных запуска от offline fixture
  `(223325,278475)`. Tracked probe читает исходный `.map`, дилатирует block
  grid на capsule radius 34 см и в порядке направлений `+X,+Y,-X,-Y`
  выбирает первый 500-см cardinal segment, для которого каждая закрытая
  half-meter cell, пересекающая swept disk capsule, существует и имеет
  block-bit 0. Для blocked case в том же порядке направлений и затем
  расстояний `1..40` half-meter cells выбирается первый ray, где swept cells
  до конечной свободны, а конечная cell имеет block-bit 1. Отсутствие любого
  кандидата является ошибкой fixture;
- probe сохраняет в capture manifest source-map SHA-256, literal
  start/free-end/blocked-end, выбранные directions и соответствующий camera
  yaw; runtime не может выбрать другую точку. Перед каждым из двух запусков
  offline DB fixture устанавливает literal start и после выхода
  восстанавливает исходную строку;
- на free segment персонаж проходит не меньше 500 см, server/client endpoint
  расходятся не более чем на 20 см и нет `FAILEDACTION`; в blocked run
  удерживается `W` при зафиксированном camera yaw, приходит terminal `BLOCK`,
  pending очищается, удержанная клавиша за следующие две секунды не создаёт
  MOVE, а после release/re-press создаётся ровно один новый MOVE.

Parity-capture имеет фиксированный контракт:

- обе игры рендерят world viewport 1920×1080, aspect 16:9;
- отдельный offline fixture ставит `Test195126` на
  `garner (223325,278475)`, angle 90° только при `login_status=0`, перед
  изменением сохраняет исходную строку и после обоих запусков восстанавливает
  её; live/production DB является fatal error;
- оба клиента подтверждают через собственный runtime state, что персонаж
  действительно вошёл в `(223325,278475)`, а не используют только ожидаемое
  значение из capture script;
- camera target height: 100 см;
- arm: 6103,2778 см;
- pitch: −55,00798°;
- horizontal FOV: 54,0222067°;
- camera yaw: 90°;
- spring collision: off;
- terrain ambient: `(1.0,1.0,1.0)`;
- scene-object lighting выбирается по `MapTile.Island` каждого anchor;
  пять literal landmarks ниже находятся на `Island=1`, где source area-light
  direction равен `(-1,-1,-1)` и `envColor=0xFF72949B`;
- Bell Tower ID 22 имеет flags `enable_env/point/shade=0/0/0`, поэтому для
  него area light отключён и используется legacy-unlit pass;
- Bench 01 ID 314 и Garden 01 ID 323 имеют flags `1/0/0`: для них UE
  `DirectionalLightComponent.GetDirection()` равен
  `(-1,+1,-1)/sqrt(3)`, а object ambient RGB равен
  `(0x72,0x94,0x9B)/255`;
- остальные видимые модели используют собственные catalog flags; при
  `shade_flag=1` checker дополнительно сверяет anchor tile RGB565, а при
  `enable_point_light=1` — IDs и параметры двух выбранных point lights на
  фиксированном capture tick;
- все видимые объекты `Island=2` получают параметры area 2, а не area 1;
- projected bounds объектов с `legacy-inherited-outside-capture` не
  пересекают world viewport;
- auto exposure, bloom, color grading, fog, clouds и motion blur: off;
- original и UE работают на фиксированных 30 simulation ticks/s; capture
  выполняется на tick 120 после четырёх секунд неподвижного состояния.

Screen projection checker использует literal source anchors:

- Bell Tower ID 22: `(223200,277320)`;
- Bench 01 ID 314: `(222610,278550)` и `(223880,278550)`;
- Garden 01 ID 323: `(222550,278180)` и `(223920,278170)`.

Ожидаемая screen position вычисляется независимой camera-matrix fixture, а
не actor helper; отклонение центра landmark не выше 5 pixels.

Orchestrator обязан получить два разных файла:
`original-223325-278475-1920x1080.png` через встроенный PrintScreen
оригинального клиента и `ue-223325-278475-1920x1080.png` через UE
high-resolution screenshot. Общий manifest хранит SHA-256 обоих кадров,
literal viewport rectangle и маску original HUD; один UE screenshot не может
закрыть gate. Кадры сохраняются рядом и показываются бок о бок.

В принятом кадре центральное окно 41×41 клетка имеет исходные stone/grass
layer IDs и baked hashes, фонтан, цветники, лавки, фонари и лестница не
перекрыты ложными palace/bank meshes. Визуальный просмотр является
обязательным дополнением к числовым gates, а не их заменой.

## Продолжение после reference-зоны

1. Восстановить click-to-move, block/region pathfinding и `ATTR_MSPD`.
2. Параллельно расширить paged-bake и строгие unresolved checks на всю
   Garner.
3. Добавить исходный sea pass и dynamic terrain point-light tint.
4. Разделить LAB ranges на idle/run и исправить character facing.
5. Перенести session в `GameInstance` и реализовать map travel.
6. Портировать type-1 particles/effects.
7. Повторить terrain/placement pipeline на остальных картах.
8. Настроить remaster-lighting поверх уже доказанной геометрии и материалов.

Финальная цель не меняется: циклы продолжаются, пока новая игра визуально и
функционально не соответствует оригинальному эталону.
