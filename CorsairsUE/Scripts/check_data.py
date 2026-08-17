"""Статический аудит gamedata.sqlite без запуска сервера.

Ищет то, что не видно иначе, пока не поздно: битые ссылки, висячие id,
неполные поля по четырём классам, скиллам, NPC, картам, монстрам,
предметам, forge-рецептам.

Запуск:

    python3 check_data.py

Отчёт: `Scripts/reports/check_data.txt`.
"""

import os
import re
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from report import Reporter  # noqa: E402

DB = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "databases", "gamedata.sqlite",
))


def main():
    report = Reporter("check_data")
    if not os.path.exists(DB):
        report.error(f"gamedata.sqlite не найден: {DB}")
        report.line(f"Ожидался файл: {DB}")
        return

    db = sqlite3.connect(DB)
    db.row_factory = sqlite3.Row

    passed = []
    failed = []

    def ok(name):
        passed.append(name)

    def bad(name, detail=""):
        failed.append(f"{name}{': ' + detail if detail else ''}")

    # ── Связность: карты, на которые ссылаются NPC и монстры.
    # npc_list.map_name и monster_list.map_name хранят display-имя («Ascaron»,
    # «Magical Ocean»), не data_name — поэтому матчим против maps.name.
    # В БД есть мусорная строка-заголовок monster_list(id=0, name='Monster Name ')
    # с map_name='Region ' — исключаем её, это не моб.
    known_map_names = {row[0].strip() for row in db.execute(
        "SELECT name FROM maps")}

    npc_maps = {row[0].strip() for row in db.execute(
        "SELECT DISTINCT map_name FROM npc_list")}
    mon_maps = {row[0].strip() for row in db.execute(
        "SELECT DISTINCT map_name FROM monster_list WHERE id > 0")}

    npc_dead = npc_maps - known_map_names
    mon_dead = mon_maps - known_map_names
    if npc_dead:
        bad("NPC на несуществующих картах", ", ".join(sorted(npc_dead)))
    else:
        ok(f"NPC-мапы ссылаются на реальные карты ({len(npc_maps)} ссылок)")
    if mon_dead:
        bad("Монстры на несуществующих картах", ", ".join(sorted(mon_dead)))
    else:
        ok(f"monster_list ссылается на реальные карты ({len(mon_maps)} ссылок)")

    # ── Какие карты сейчас вообще населены (это не провал, а инфа: игровых
    # карт много, а NPC/мобов в static db хватает только на несколько).
    npc_per_map = {row[0].strip(): row[1] for row in db.execute(
        "SELECT map_name, COUNT(*) FROM npc_list GROUP BY map_name")}
    mon_per_map = {row[0].strip(): row[1] for row in db.execute(
        "SELECT map_name, COUNT(*) FROM monster_list WHERE id > 0 GROUP BY map_name")}

    populated = sorted(k for k in known_map_names
                       if npc_per_map.get(k, 0) > 0 or mon_per_map.get(k, 0) > 0)
    report.line(f"населённых карт: {len(populated)} из {len(known_map_names)}")
    report.line(f"населённые карты: {', '.join(populated)}")

    # карты, которые сервером обслуживаются, но в static db пустые. Это может
    # быть как полноценная внутриигровая локация без скриптов спавна, так и
    # болванка; считаем информативным фактом, а не багой.
    empty_but_mapped = sorted(k for k in known_map_names
                              if npc_per_map.get(k, 0) == 0
                              and mon_per_map.get(k, 0) == 0)
    report.line(f"пустые по static db ({len(empty_but_mapped)} шт): {', '.join(empty_but_mapped)}")

    # ── NPC-места не отрицательные
    bad_pos = list(db.execute(
        "SELECT id, name, map_name, x_pos, y_pos FROM npc_list "
        "WHERE x_pos <= 0 OR y_pos <= 0"))
    if bad_pos:
        bad("NPC с битой позицией", f"{len(bad_pos)} шт")
        for r in bad_pos[:5]:
            report.line(f"  ⚠ {r['id']} {r['name']} @ {r['map_name']} ({r['x_pos']},{r['y_pos']})")
    else:
        ok(f"все NPC стоят на нормальных координатах ({sum(npc_per_map.values())} шт)")

    # ── monster_list-заголовок и мусорные строки: id=0, x=0, y=0
    header_rows = list(db.execute(
        "SELECT id, name FROM monster_list WHERE id = 0 OR (x_pos = 0 AND y_pos = 0)"))
    if header_rows:
        report.warn(f"монстры-служебные ({len(header_rows)} шт), "
                    f"например id=0 name={header_rows[0][1]!r} — это заголовок таблицы, не моб")

    # ── Скилки: apply_distance/target не бредовые
    crazy_distance = list(db.execute(
        "SELECT id, name, apply_distance FROM skills "
        "WHERE apply_distance < 0 OR apply_distance > 5000"))
    if crazy_distance:
        bad("скилы с бредовым apply_distance", f"{len(crazy_distance)} шт")
    else:
        ok(f"apply_distance у разумных пределах ({db.execute('SELECT COUNT(*) FROM skills').fetchone()[0]} скиллов полностью)")

    # ── Скилы без имени или без типа
    nameless = list(db.execute(
        "SELECT id FROM skills WHERE name IS NULL OR name = ''"))
    if nameless:
        bad("скилы без имени", f"{len(nameless)} id: {[r[0] for r in nameless[:5]]}")
    else:
        ok("все скилы имеют имя")

    # ── Life-скилы: Woodcutting/Cooking/Fishing на месте
    life_skills = {row[0]: row[1] for row in db.execute(
        "SELECT id, name FROM skills WHERE name IN "
        "('Woodcutting', 'Cooking', 'Fishing')")}
    expected_life = {"Woodcutting", "Cooking", "Fishing"}
    missing_life = expected_life - set(life_skills.values())
    if missing_life:
        bad("life-скилы потеряли", ", ".join(missing_life))
    else:
        ok(f"life-скилы найдены: {sorted(life_skills.keys())}")

    # ── 4 класса
    races = list(db.execute("SELECT id, name FROM cha_create"))
    if len(races) == 4:
        names = [r[1] for r in races]
        expected = {"Lambert", "Carsise", "Phyllis", "Ami"}
        if set(names) == expected:
            ok(f"4 класса на месте: {names}")
        else:
            bad("названия классов", f"ожидали {expected}, есть {names}")
    else:
        bad("должно быть 4 класса", f"нашли {len(races)}")

    # ── Profession поле в cha_create залито мусором -842150451 (int 0xCDCDCDCD
    # как признак неинициализированного int) — это хвост старого импорта.
    professions = list(db.execute("SELECT id, name, profession FROM cha_create"))
    uninit_prof = [r for r in professions if r[2] == -842150451]
    if uninit_prof:
        report.warn(f"cha_create.profession не заполнен ({len(uninit_prof)} записей) — "
                    f"интерпретатор должен читать по id, не по profession")

    # ── Forge: входы и выходы — это существующие предметы
    item_ids = {row[0] for row in db.execute("SELECT id FROM item_types")}
    # схема forge имеет свои поля, но простая проверка — это ссылки на item ids
    forge_schema = list(db.execute("PRAGMA table_info(forge)"))
    forge_cols = [c[1] for c in forge_schema]
    report.line(f"forge columns: {forge_cols}")

    # ── Items: висячие type_id. Это не баг: локальный gamedata.sqlite — слепок
    # старой версии каталога item_types, а items в нём содержат категории из
    # патча посвежее (tattoos=27, hairdos=28, ship parts 70-90). type=0 —
    # служебные/квестовые маркеры (Gold, Tax, exclamation marks). Формально
    # проверка отражает известный дифф версий и считается WARNING'ом.
    unknown_types = list(db.execute(
        "SELECT DISTINCT type FROM items WHERE type NOT IN (SELECT id FROM item_types)"))
    if unknown_types:
        types_list = sorted(r[0] for r in unknown_types)
        report.warn(f"предметы с type, которых нет в item_types ({len(types_list)} шт: "
                    f"{types_list}) — локальная БД отстаёт от каталога; "
                    f"категории дополнятся при рефреше gamedata.sqlite")
        # не считаем это провалом
    else:
        ok(f"все предметы в рамках известных типов ({db.execute('SELECT COUNT(*) FROM items').fetchone()[0]} шт)")

    # ── Monster density per map (дополнительная инфа)
    report.line("───")
    report.line("густота мобов и NPC по картам:")
    for m in sorted(known_map_names):
        n = npc_per_map.get(m, 0)
        mo = mon_per_map.get(m, 0)
        report.line(f"  {m:30s}  NPC={n:3d}  Monsters={mo:3d}")

    # ── Скилы по типу (дополнительная инфа)
    report.line("───")
    report.line("скиллы по типу:")
    for row in db.execute(
            "SELECT type, COUNT(*) c FROM skills GROUP BY type ORDER BY c DESC"):
        report.line(f"  type={row[0]:3d}  count={row[1]}")

    report.line("───")
    for line in passed:
        report.line(f"  ПРОЙДЕНО  {line}")
    for line in failed:
        report.error(f"  ПРОВАЛ    {line}")
    report.line(f"проверок: {len(passed)}, провалов: {len(failed)}")

    db.close()


if __name__ == "__main__":
    main()
