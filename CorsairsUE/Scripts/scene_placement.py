"""Общая часть расстановки объектов карты.

Модуль появился потому, что расстановка живёт в двух скриптах: `place_objects`
ставит одну карту, `place_all_maps` — все сразу за один запуск редактора. Пока
логика была скопирована в оба, копии разошлись: привязка построек к рельефу
попала только в первый, и все карты, собранные вторым, встали на голое
смещение. Здесь лежит то, что обязано совпадать.

Спавн сюда не вынесен намеренно: один скрипт собирает инстансы группами и
считает границы карты, другой печатает подробный разбор по мешам. Общее у них —
какие объекты ставить и куда, а не как именно.
"""

import os
import struct

# Высота на диске — знаковый байт в единицах по десять сантиметров, при
# экспорте растянутый в беззнаковое шестнадцатибитное поле формулой
# (raw + 128) * 256. Здесь выполняется обратное преобразование.
HEIGHT_BIAS = 128
HEIGHT_SCALE = 256
CENTIMETERS_PER_UNIT = 10.0
UNITS_PER_CELL = 100.0


class TerrainHeights:
    """Карта высот одной игровой карты.

    Высота объекта в оригинале — это высота рельефа под ним плюс собственное
    смещение (`d.z = GetGridHeight(x, y)` в SceneSign.cpp). Без первого
    слагаемого постройки стоят на голом смещении: на склонах они уходят в
    землю или повисают в воздухе, и город идёт ступеньками.
    """

    def __init__(self, path):
        self.cells = None
        self.side = 0
        if not os.path.exists(path):
            return
        with open(path, "rb") as handle:
            raw = handle.read()
        count = len(raw) // 2
        side = int(round(count ** 0.5))
        if side * side != count:
            return
        self.cells = struct.unpack(f"<{count}H", raw)
        self.side = side

    def at(self, world_x, world_y):
        """Высота земли в мировой точке, в сантиметрах."""
        if not self.side:
            return 0.0
        col = min(max(int(world_x // UNITS_PER_CELL), 0), self.side - 1)
        row = min(max(int(world_y // UNITS_PER_CELL), 0), self.side - 1)
        stored = self.cells[row * self.side + col]
        return (stored // HEIGHT_SCALE - HEIGHT_BIAS) * CENTIMETERS_PER_UNIT


def load_heights(script_dir, map_name):
    """Читает карту высот по имени карты. Отсутствие файла — не ошибка."""
    path = os.path.join(script_dir, "..", "Data", "Heights",
                        f"{map_name}.height.r16")
    return TerrainHeights(path), path


def split_effects(objects):
    """Разделяет записи манифеста на постройки и эффекты.

    Различает их поле `type` — старшие два бита `sTypeID` исходного файла
    (0 — объект сцены, 1 — эффект). Оригинал разводит их по разным ветвям
    (`GetType()` в GameAppMsg.cpp): объект ищется в таблице сцены, эффект — в
    таблице эффектов, и нумерация у них независимая. Без разделения эффект с
    идентификатором 1 подставляет постройку с идентификатором 1 — а таких
    эффектов на garner больше двух тысяч, и весь город обрастает лишними
    копиями одного дома.
    """
    scene, effects = [], []
    for obj in objects:
        (scene if obj.type == 0 else effects).append(obj)
    return scene, effects


def object_location(library, obj, heights):
    """Точка постройки в мире: положение по манифесту плюс высота земли."""
    location = library.get_object_location(obj, 100.0)
    if heights is None or not heights.side:
        return location
    import unreal
    return unreal.Vector(location.x, location.y,
                         location.z + heights.at(obj.x, obj.y))


def effect_summary(effects):
    """Сводка отложенных эффектов: сколько всего, каких видов, самые частые."""
    by_id = {}
    for obj in effects:
        by_id[obj.model_id] = by_id.get(obj.model_id, 0) + 1
    top = sorted(by_id.items(), key=lambda kv: -kv[1])[:5]
    return len(effects), len(by_id), top
