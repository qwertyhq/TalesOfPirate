# Отчёт Kimi K3: играбельный Garner целиком (этап 2)

Ветка `kimi/garner-playable` (развилка от 452fd6b6). Работа шла только в
`/Users/ivan/code/TalesOfPirate-kimi-garner`.

## Итог приёмки

| Критерий | Статус | Свидетельство |
|---|---|---|
| Полный манифест garner | ок | `artifacts/garner.scene.json.objects.json`: records=50017, scene=46991, effects=3026, reference=1634 — константы конвертера сошлись |
| Массовая расстановка ~47k | ок | `reports/place_garner_city.txt`: anchors=46980, staticInstances=89352 ISM в 947 актёрах, скелетных актёров 2687, страниц 64 |
| Коллизии | частично | страницы рельефа — CTF_USE_COMPLEX_AS_SIMPLE (`reports/import_garner_terrain.txt`); постройки без коллизий — gameplay-проходимость идёт через half-meter grid + сервер, стены актёров капсулу не держат |
| Стартовая карта / автологин | ок | `DefaultEngine.ini`: GameDefaultMap/EditorStartupMap=`/Game/Maps/Garner`, GameMode=CorsairsGameMode; Data/Heights/garner.* на месте |
| check_login | зелёный | `reports/check_login.txt`: «УСПЕХ: клиент в мире, движение подтверждено сервером», учётка bot |
| Кадр против эталона | численно | `artifacts/visual-progress/garner-city-full-kimi-20260810-3.png` (1920×1080, parity-точка фонтана); численная сверка ниже; визуальной сверкой глазами я не располагаю — просьба посмотреть самим |
| FPS ≥ 60 | НЕ измерено | кадр собирается ~60 с с компиляцией шейдеров; реальный FPS в городе не мерял |

## Что сделано по шагам

1. **Манифест.** Мой конвертер собран заново (cmake), `scene-manifest Client/map/garner.map Client/map/garner.obj` —
   fail-closed константы прошли с ходу: records=50017 models=46991 deferred=3026 reference=1634,
   острова {0:3,1:1625,2:6}.
2. **Инвентаризация.** В манифесте 317 уникальных modelId; model-map покрывает 315. Дыры: id=91 и id=92
   (4+7=11 placements) — их нет даже в `scene_objects`, молчаливого сокращения нет: фильтр в
   `place_garner_city.py` считает их явно и fail-closed на ином количестве.
3. **Конвертация корпуса.** Сначала 32 модели падали на строгой валидации материалов
   (alpha test одновременно со смешиванием; FILTER с парой ONE/ONE). По исходнику движка
   (`lwMtlTexAgent::BeginSet`, sources/Engine/Resource/ResourceMgr.cpp:1786) DX9 применяет
   состояния из файла без взаимного отсева, тест отсекает пиксели до смешивания — это — легальные
   комбинации. Ослабил резолвер (`tools/AssetConverter/src/LgoParser.cpp`), расширил тесты
   (9→11 кейсов), полный ctest 4/4 зелёный. Итог: 637 OK, остались 2 ANIM_BLOCK_MALFORMED
   (nml-bd137, nml-bd200 — NaN-веса скина) — garner не использует.
   19 моделей с TEXIMG/MTLOPACITY-контроллерами capture-bake не принимает — сконвертированы
   без `--legacy-capture-tick` и влиты (их анимация текстур/прозрачности снята: в отчете видно).
4. **Импорт.** 2017 glTF → `/Game/GarnerCity` (10314 ассетов, Interchange без ошибок).
   Материальный прогон: 4069 MIC перепривязаны к пяти Unlit-parents с CPD[0..32], census
   opaque=2062 masked=1626 alpha=7 additive=326 subtractive=48 (subtractive=48 сходится с
   обещанием parity-плана 1:1 — негативная верификация, что резолвер не «размыл» режимы).
   Обёртка `apply_garner_city_material_modes.py`, regex имён расширен под многоточечные имена
   ('ocean_h.01.bmp0' → 'ocean_h_01_bmp0').
5. **Рельеф.** 64 страницы обычной каталожной конвертацией (базис Q по умолчанию) →
   `/Game/Terrain/GarnerCityQ` + CTF_USE_COMPLEX_AS_SIMPLE. Материал — dominant-texture MI
   из `Data/terrain_tile_textures.json` (покрыто 60/64 страниц; 4 страницы без записи
   остались на дефолтном материале меша — в отчёте явные имена: 00_06, 06_02, 07_02, 07_03).
6. **Расстановка.** `place_garner_city.py`: транзакция карты (temp → publish → backup delete
   с компенсацией молчаливого delete_asset), фильтрация type=0 минус дыры 91/92, lighting
   payloads через `scene_lighting` для всех 46980 (синтетический manifest с
   referenceObjectCount=len — резолюция не трогалась). Статика: один ISM-актор на уникальный
   меш, per-instance custom data 33 floats через set_custom_data_value (probe подтвердил API).
   Skeletal: актёры с замороженной позой (capture sampleFrame/30 где метаданные есть, иначе 0).
   PlayerStart — source-точка фонтана, земля из half-meter raster.
7. **Свет.** `setup_lighting.py Garner`: солнце 8 lux, SkyAtmosphere/SkyLight, туман 0.008,
   фиксированная экспозиция, force_no_precomputed_lighting.
8. **Старт.** GameDefaultMap/EditorStartupMap=`/Game/Maps/Garner`, GlobalDefaultGameMode=CorsairsGameMode.

## Численная сверка кадра

Одинаковая камера у фонтана (source eye 223325,281975,5100 → цель 223325,278475,100).
Я глазами картинки не вижу (нет image input), поэтому только численность:

- яркость по строкам после света: полный город ~135–155/строка против ~117–163 у parity-кадра
  четверти — диапазоны совпадают (без света мой вариант был вдвое темнее);
- «красные» пиксели (мягкое правило r>120, r>1.4g, r>1.4b) в parity-кропе (648,0,1599,639):
  эталон живая клиент 0.4955%, parity-четверть 0.0938%, мой полный город 0.7385% —
  город вокруг четверти достроился, перекоса нет;
- по всему кадру сильно отличающихся от parity-четверти пикселей 44.9% — остальной город вокруг;
  для целевого суждения нужна визуальная сверка ориентиров.

## Найденные грабли (новые, за этапом 2)

1. **`-run=pythonscript` и полный UnrealEditor: съёмка.** Бинарь `/Binaries/Mac/UnrealEditor`
   сам перезапускается в `.app` и закрывается; полноценная съёмка с нашего коммандлета выглядит
   exit=1/0 без кадра и без python-строк. Рабочий шаблон: nohup + positional map +
   `-ExecutePythonScript=` + **`set_keep_python_script_alive(True)` ДО синхронного конца скрипта**;
   без него плагин шлёт QUIT_EDITOR через ~7 с и fallback-кадр не пишется. Кадр снимается
   за ~60–70 с с компиляцией шейдеров.
2. **`--legacy-capture-tick` обязателен при корпусе с capture extras:** без него policy
   preserveAnimated не пишется и preflight/import счёт не сойдётся.
3. **TEXIMG/MTLOPACITY-контроллеры + capture-tick несовместимы** — capture-bake отказывается
   молча их ронять. Окольный путь: конвертация таких моделей без capture-tick.
4. model-map: `build_model_map.py` привязывает пути к contentRoot из JSON — после переписывания
   верхнего `contentRoot` нужно переписывать и все парти-пути, иначе молча читается legacy ns.
5. Материалы: имена с несколькими точками ('ocean_h.01.bmp0') существуют вне эталонной зоны —
   regex в apply_scene_material_modes.py расширен (совместимо со старым), тесты не задеты.

## Осмысленные отступления/решения

- Резолвер материалов ослаблен под поведение оригинального движка (тест+смешивание, FILTER+ONE/ONE → Additive,
  test+blend → Alpha). Обоснование в коммите 1d9797a5. Это меняет fail-closed поведение этапа 1,
  намеренно: эталонная зона материалов не затрагивалась, count тестов прошёл.
- Коллизии на постройках не выставлял: gameplay-проходимость — через блокировку клеток в земле
  (Data/Heights/garner.block.raw), и walkability сервера их тоже использует.
- FPS не мерял — не нашёл честного способа снять метрику без ручной игры. На M-серии при
  947 ISM-компонентах ожидаю запас, но это ожидание, не факт.

## Пропущенные модели (итоговый честный список дыр)

| modelId | имя | placements | причина |
|---|---|---|---|
| 91 | (нет в scene_objects) | 4 | нет каталога |
| 92 | (нет в scene_objects) | 7 | нет каталога |
| итого | | 11 из 46991 (0.023%) | |

Анимированная температура TEXIMG/MTLOPACITY у 19 моделей (nml-bd120, nml-bd143 — 11 placements в сумме) —
конвертированы, но без capture-aнимации.

## Артефакты

- `artifacts/garner.scene.json.objects.json` — манифест (не в гите)
- `artifacts/garner_model_map.json` — modelId → /Game/GarnerCity parts (не в гите)
- `artifacts/models-scene/` — 2017 glTF корпуса (не в гите)
- `artifacts/visual-progress/garner-city-full-kimi-20260810-3.png` — кадр
- Отчёты: `Scripts/reports/{place_garner_city,import_garner_terrain,apply_garner_city_material_modes,check_login,setup_lighting}.txt`
