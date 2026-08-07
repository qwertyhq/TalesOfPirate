"""Настраивает освещение уровней.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/setup_lighting.py [карта ...]"

Без аргументов обрабатывает все карты в `/Game/Maps`.
Отчёт: `Scripts/reports/setup_lighting.txt`.

Что ставится и зачем:

- **Directional Light** — солнце. Подвижное, потому что запечь свет на карте с
  полумиллионом инстансов невозможно: расчёт занял бы часы, а карт сорок семь.
  Помечено источником света атмосферы: без этого небо не знает, где солнце, и
  остаётся равномерно серым.
- **Sky Atmosphere** — небо и рассеивание. Даёт цвет неба и подсветку теней.
- **Sky Light** — отражённый свет неба. В режиме реального времени, иначе
  требует захвата, которого в headless-режиме не сделать.
- **Exponential Height Fog** — воздушная перспектива. На карте шириной четыре
  километра без неё дальние здания выглядят приклеенными к небу.
- **Volumetric Cloud** — облака. Небо без них читается как градиент.
- **Post Process Volume** — прежде всего фиксированная экспозиция. По
  умолчанию UE подстраивает яркость под сцену, и при повороте камеры картинка
  «дышит»; на пустых участках карты это доходит до полной засветки.

Уровню выставляется признак отказа от предрасчитанного света: иначе редактор
считает освещение устаревшим и рисует предупреждение поверх экрана.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter, RefuseIfEditorOpen      # noqa: E402

LEVEL_ROOT = "/Game/Maps"

# Яркость солнца в люксах. Значение по умолчанию в UE рассчитано на сцены с
# физически корректными материалами; наши текстуры пришли из движка, где
# освещение было простым, поэтому солнце приглушено. Но вчетверо приглушённое
# делало постройки серыми — текстуры на них просто не читались.
SUN_INTENSITY = 8.0
SUN_ROTATION = unreal.Rotator(0.0, -42.0, 35.0)

# Экспозиция фиксируется одним значением: равные минимум и максимум отключают
# автоподстройку.
EXPOSURE = 1.0

# Плотность тумана. Подобрана под масштаб карты: заметная воздушная
# перспектива на километре без затягивания ближних объектов.
FOG_DENSITY = 0.008
FOG_HEIGHT_FALLOFF = 0.05



def SetIfPresent(report, target, name, value):
    """Ставит свойство, если оно есть у объекта.

    Имена свойств между версиями движка меняются, а отсутствие одного из них
    не повод бросать настройку остальных: без этого одна переименованная
    мелочь оставляла бы уровень вовсе без света.
    """
    try:
        target.set_editor_property(name, value)
        return True
    except Exception:                                # noqa: BLE001
        report.warn(f"свойство '{name}' не найдено — пропущено")
        return False


def find_or_spawn(actor_class, label, location=None):
    """Возвращает актёра нужного класса, создавая при отсутствии.

    Повторный запуск не должен плодить вторые солнца.
    """
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in subsystem.get_all_level_actors():
        if actor.__class__ == actor_class:
            return actor, False

    spawned = subsystem.spawn_actor_from_class(
        actor_class,
        location if location is not None else unreal.Vector(0.0, 0.0, 20000.0),
        unreal.Rotator(0.0, 0.0, 0.0))
    if spawned is not None:
        spawned.set_actor_label(label)
    return spawned, True


def setup_sun(report):
    sun, _ = find_or_spawn(unreal.DirectionalLight, "Sun")
    if sun is None:
        report.warn("солнце не создано")
        return

    sun.set_actor_rotation(SUN_ROTATION, False)

    # Компонент берётся по классу, а не по имени свойства: у актёров света
    # оно называется light_component, и обращение по типу устойчивее к
    # различиям между классами.
    component = sun.get_component_by_class(unreal.DirectionalLightComponent)
    if component is None:
        report.warn("у солнца нет компонента света")
        return
    # Подвижный источник: запекать свет на полумиллионе инстансов нереально.
    SetIfPresent(report, component, "mobility", unreal.ComponentMobility.MOVABLE)
    SetIfPresent(report, component, "intensity", SUN_INTENSITY)
    # Без этого признака небо не знает, где солнце, и остаётся серым.
    SetIfPresent(report, component, "atmosphere_sun_light", True)
    SetIfPresent(report, component, "cast_shadows", True)
    SetIfPresent(report, component, "dynamic_shadow_distance_movable_light", 20000.0)


def setup_sky(report):
    atmosphere, _ = find_or_spawn(unreal.SkyAtmosphere, "SkyAtmosphere")
    if atmosphere is None:
        report.warn("атмосфера не создана")

    sky, _ = find_or_spawn(unreal.SkyLight, "SkyLight")
    if sky is None:
        report.warn("свет неба не создан")
        return

    component = sky.get_component_by_class(unreal.SkyLightComponent)
    if component is None:
        report.warn("у света неба нет компонента")
        return
    SetIfPresent(report, component, "mobility", unreal.ComponentMobility.MOVABLE)
    # Захват в реальном времени: обычный требует запуска захвата, которого в
    # headless-режиме не сделать.
    SetIfPresent(report, component, "real_time_capture", True)
    SetIfPresent(report, component, "intensity", 1.0)


def setup_fog(report):
    fog, _ = find_or_spawn(unreal.ExponentialHeightFog, "HeightFog",
                           unreal.Vector(0.0, 0.0, 0.0))
    if fog is None:
        report.warn("туман не создан")
        return

    component = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    if component is None:
        report.warn("у тумана нет компонента")
        return
    SetIfPresent(report, component, "fog_density", FOG_DENSITY)
    SetIfPresent(report, component, "fog_height_falloff", FOG_HEIGHT_FALLOFF)


def setup_clouds(report):
    clouds, _ = find_or_spawn(unreal.VolumetricCloud, "Clouds",
                              unreal.Vector(0.0, 0.0, 0.0))
    if clouds is None:
        report.warn("облака не созданы")


def setup_post_process(report):
    volume, created = find_or_spawn(unreal.PostProcessVolume, "PostProcess",
                                    unreal.Vector(0.0, 0.0, 0.0))
    if volume is None:
        report.warn("объём постобработки не создан")
        return

    # Безграничный объём: иначе настройки действуют лишь внутри его коробки, а
    # игрок ходит по всей карте.
    SetIfPresent(report, volume, "unbound", True)

    settings = volume.settings
    SetIfPresent(report, settings, "override_auto_exposure_min_brightness", True)
    SetIfPresent(report, settings, "auto_exposure_min_brightness", EXPOSURE)
    SetIfPresent(report, settings, "override_auto_exposure_max_brightness", True)
    SetIfPresent(report, settings, "auto_exposure_max_brightness", EXPOSURE)
    volume.set_editor_property("settings", settings)


def setup_world_settings(report):
    world = unreal.EditorLevelLibrary.get_editor_world()
    settings = world.get_world_settings()
    # Предрасчитанного света нет и не будет: иначе редактор считает освещение
    # устаревшим и рисует предупреждение поверх экрана.
    SetIfPresent(report, settings, "force_no_precomputed_lighting", True)


def process_level(report, level_path):
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.load_level(level_path)

    setup_sun(report)
    setup_sky(report)
    setup_fog(report)
    setup_clouds(report)
    setup_post_process(report)
    setup_world_settings(report)

    subsystem.save_current_level()


def main(report):
    if RefuseIfEditorOpen(report):
        return

    only = set(sys.argv[1:])

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    levels = sorted(str(a.asset_name)
                    for a in registry.get_assets_by_path(unreal.Name(LEVEL_ROOT),
                                                         recursive=True)
                    if str(a.asset_class_path.asset_name) == "World")

    report.line(f"уровней найдено: {len(levels)}")

    done = 0
    for name in levels:
        if only and name not in only:
            continue
        process_level(report, f"{LEVEL_ROOT}/{name}")
        done += 1

    report.line(f"УРОВНЕЙ НАСТРОЕНО: {done}")
    if done == 0:
        report.error("ПРОВАЛ: ни один уровень не настроен")


report = Reporter("setup_lighting")
try:
    main(report)
except Exception as exc:                             # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
