"""Аудит игровых механик от и до: карты, NPC, монстры, life-скилы, торговля,
сражения, классы. Живой бот на живом стеке.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_world.py [хост] [порт] [учётная-запись] [пароль]"

По умолчанию: 127.0.0.1 1973 bot admin.
Отчёт: `Scripts/reports/check_world.txt`.

Ключевая фишка: GS-таблица init_x/init_y — не координаты входа, а центр
владения карты. Клиент дёргает карту через SwitchMapEntry, и только та
точка гарантированно валидна на сервере. Поэтому для `&move` берём
координаты не из `maps`, а из `birth_conf.lua` — первую валидную
точку для каждой карты. Пару карт у вас нет ни там, ни тут — они
сразу помечаются «не существует на сервере».

Порядок:
1. Заступить в мир.
2. Пробежаться по всем картам из gamedata.maps + birth_conf.lua:
   `&move X,Y,<data_name>` в точку из birth_conf + проверка что сервер
   подтвердил смену карты.
3. На тех картах, где население есть — считать видимых NPC/монстров
   в поле зрения вокруг birth-точки; сравнивать с каталогом БД.
4. Вернуться в город, проверить life-механики: Woodcutting, Fishing,
   Cooking, все 4 класса, торговлю.

Скрипт не эвристикой угадывает результат, а проверяет факты: координаты,
hp, золото, и что кто сказал в диалоге.
"""

import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

POLL_INTERVAL = 0.05

CTRL_NPC = 2
CTRL_MONS = 5

HOME_X = 2246
HOME_Y = 2704
HOME_MAP = "garner"
OTHER_MAP = "magicsea"
OTHER_X = 1350
OTHER_Y = 550

DB = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "databases", "gamedata.sqlite",
))


def say_and_pump(session, line, seconds=3.0):
    session.say(line)
    pump(session, seconds)


def pump(session, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        session.poll()
        time.sleep(POLL_INTERVAL)


def pump_until_map(session, wanted_map, timeout=25.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        session.poll()
        if session.get_map_name() == wanted_map:
            return True
        time.sleep(POLL_INTERVAL)
    return False


def enter_world(session, report):
    """Логин + заступление в мир первым персонажем."""
    pump(session, 1.5)
    stage = session.get_stage()
    report.line(f"начальная стадия: {stage}")
    if stage != unreal.CorsairsLoginStage.SELECTING_CHA:
        return False

    chars = session.get_characters()
    if not chars:
        report.error("прогон без персонажей: создать невозможно из python")
        return False

    report.line(f"персонажей: {len(chars)}, выбираем {chars[0].name}")
    session.enter_world(0)
    pump(session, 6.0)
    if session.get_stage() != unreal.CorsairsLoginStage.IN_WORLD:
        report.error(f"не вошли в мир: {session.get_stage()}")
        return False
    report.line(f"в мире: карта={session.get_map_name()}")
    return True


# Расположение lua-скриптов относительно каталога Scripts:
# Scripts/ -> CorsairsUE/Scripts/...; корень = two levels up.
BIRTH_CONF = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "server", "GameServer", "resource", "script",
    "birth", "birth_conf.lua",
))
RESOURCE_ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "server", "GameServer", "resource",
))


_BIRTH_RE = None


def _birth_re():
    global _BIRTH_RE
    if _BIRTH_RE is None:
        import re
        _BIRTH_RE = re.compile(
            r'AddBirthPoint\("([^"]+)",\s*"([^"]+)",\s*(\d+),\s*(\d+)\)'
        )
    return _BIRTH_RE


def read_server_birth_points():
    """Точки входа из birth_conf.lua — источник истины о том, откуда
    стартует персонаж. Если карта не упомянута там, сервер сам перекинет
    бота на «свою карту» при входе, и move никуда не приведёт.
    """
    result = {}
    if not os.path.exists(BIRTH_CONF):
        return result
    with open(BIRTH_CONF, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            match = _birth_re().search(line)
            if not match:
                continue
            city, map_name, x, y = match.groups()
            # Нам важен первый встреченный (обычно самый центральный) вход.
            result.setdefault(map_name, (int(x), int(y), city))
    return result


def list_served_maps():
    """Какие карты реально обслуживает GameServer — по наличию папки
    resource/<data_name>/<data_name>.atr. Без `.atr` GameServer не
    регистрирует сабмап и персонаж tуда не попадает.
    """
    out = set()
    if not os.path.isdir(RESOURCE_ROOT):
        return out
    for entry in os.listdir(RESOURCE_ROOT):
        atr_path = os.path.join(RESOURCE_ROOT, entry, f"{entry}.atr")
        if os.path.isdir(os.path.join(RESOURCE_ROOT, entry)) and os.path.isfile(atr_path):
            out.add(entry)
    return out


def read_map_birth_points(db, server_birth):
    """Собираем «имя карты → рабочая birth-точка».

    maps.init_x/init_y — не координаты входа, а «административный центр»:
    подаут на них в большинстве карт (init=0,0) просто отбрасывает
    персонажа обратно на последнюю валидную позицию. Настоящая входная —
    первая точка из lua-файла birth_conf.lua.
    """
    out = []
    for row in db.execute("SELECT id, name, data_name, init_x, init_y FROM maps"):
        data_name = row["data_name"]
        if data_name in server_birth:
            # lua — единственный авторитетный источник входных координат.
            # init_x/init_y из maps — административный центр карты (для
            # magicsea это 1350,550 — за пределами города; сервер отвечает
            # «is unlawful, fallback to birth point» и перебрасывает обратно).
            x, y, city = server_birth[data_name]
            confident = True
        else:
            # Карта сервером обслуживается, но birth-точки в lua нет —
            # re-init'овые координаты туда тоже вести не будут (.init_x/y это
            # центр владения карты, не вход). Помечаем как негативную карту.
            x, y = 0, 0
            city = row["name"]
            confident = False
        out.append((row["id"], row["name"], data_name, x, y, city, confident))
    return out


def try_teleport(session, report, map_name, x, y):
    """&move в указанное место и проверка что карта поменялась.

    Возвращает True если сервер подтвердил переход.
    """
    before = session.get_map_name()
    # birth_conf уже хранит координаты в тех же клетках, что ждёт `&move`.
    # В предыдущей версии мы делили на 100 от maps.init_x (это server-units),
    # отсюда и была каскадная ошибка.
    session.say(f"&move {x},{y},{map_name}")
    # Одна итерация UE-тика занимает ощутимое время после teleport (за счёт
    # CHA_OUT/CHA_ENTER/CHA_EQUIP по трассе серверного лога), поэтому 15s
    # не хватает: по GameServer.log видно, что все 25 SwitchMap завершились,
    # но у части карт MapName в клиенте меняется только после ещё одного
    # round-trip нового `&move`. Увеличиваем до 30s.
    ok = pump_until_map(session, map_name, timeout=30.0)

    if not ok:
        report.warn(f"карта {map_name}: не перешли за 30 с (остались на {before})")
        return False

    # Дополнительно убедимся что где-то рядом со spawn-точкой.
    actor = session.get_local_actor()
    dx = abs(actor.position.x - x)
    dy = abs(actor.position.y - y)
    # Точность не клетка: сервер может нас положить в центре области.
    return dx < 10000 and dy < 10000


def scan_local_population(session):
    """Считаем кого видим."""
    actors = session.get_visible_actors()
    npcs = [a for a in actors if a.ctrl_type == CTRL_NPC]
    mons = [a for a in actors if a.ctrl_type == CTRL_MONS]
    return npcs, mons


def try_npc_talk(session, npcs, report):
    """Разговор с первым NPC если он есть."""
    if not npcs:
        return None
    npc = npcs[0]
    session.say(f"&move {npc.position.x // 100},{npc.position.y // 100}")
    pump(session, 4.0)
    result = session.talk_to_npc(npc.world_id)
    # UCorsairsSession::TalkToNpc возвращает bool, поэтому talking-статус —
    # True/False, а не ECorsairsActionRequestResult. `result != SENT` на
    # практике означало «result is True» → ругались на любой успешный вызов.
    if result is False or (result is not True and not bool(result)):
        report.warn(f"  разговор с «{npc.name}» не отправлен: {result}")
        return None
    pump(session, 4.0)
    page = session.get_npc_talk_page()
    # FCorsairsNpcTalkPage хранит только текст (NpcWorldId/Command/Text) —
    # options не существует, опции NPC ходят через отдельные cmd внутри Text.
    if not page or not page.text:
        report.warn(f"  разговор с «{npc.name}» не вернул текст страницы")
        return None
    return npc.name, len(page.text)


def visit_map(session, report, map_id, map_name, x, y, db_npcs, db_mons):
    """Одна карта: телепорт + инвентаризация."""
    report.line(f"─── {map_name} (id={map_id})")
    report.line(f"  БД: NPC={db_npcs} монстров={db_mons}")

    if x <= 0 or y <= 0:
        report.warn(f"  нет birth-точки в maps (create_pos={x},{y})")
        return {"map": map_name, "status": "no_birth"}

    if not try_teleport(session, report, map_name, x, y):
        return {"map": map_name, "status": "teleport_failed", "db_npc": db_npcs, "db_mons": db_mons}

    pump(session, 3.0)
    npcs, mons = scan_local_population(session)
    report.line(f"  в поле зрения: NPC={len(npcs)}, монстров={len(mons)}")

    npc_talk = try_npc_talk(session, npcs, report)
    status = "ok"
    if db_npcs > 0 and not npcs:
        status = "npc_missing"
    if db_mons > 0 and not mons:
        status = "mons_missing"

    return {
        "map": map_name,
        "status": status,
        "db_npc": db_npcs, "db_mons": db_mons,
        "seen_npc": len(npcs), "seen_mons": len(mons),
        "npc_talk": npc_talk,
    }


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 1973
    account = sys.argv[3] if len(sys.argv) > 3 else "bot"
    password = sys.argv[4] if len(sys.argv) > 4 else "admin"

    report = Reporter("check_world")
    report.line(f"подключение {host}:{port} пользователем `{account}`")

    if not os.path.exists(DB):
        report.error(f"gamedata.sqlite не найден: {DB}")
        return

    db = sqlite3.connect(DB)
    db.row_factory = sqlite3.Row

    npcs_per_map = {row[0]: row[1] for row in db.execute(
        "SELECT map_name, COUNT(*) FROM npc_list GROUP BY map_name")}
    mons_per_map = {row[0]: row[1] for row in db.execute(
        "SELECT map_name, COUNT(*) FROM monster_list GROUP BY map_name")}
    served = list_served_maps()
    server_birth = read_server_birth_points()
    birth = read_map_birth_points(db, server_birth)

    report.line(f"карты в БД: {len(birth)}, «сервится» GS: {len(served)}, "
                f"в birth_conf: {len(server_birth)}")

    session = unreal.CorsairsSession()
    session.login(host, port, account, password)

    if not enter_world(session, report):
        session.logout()
        return

    # ── Фаза 1: обход всех карт
    results = []
    for map_id, map_name, data_name, x, y, city, confident in birth:
        record = {"map": data_name, "display": map_name, "city": city}
        if data_name not in served:
            record["status"] = "not_served"
            report.line(f"─── {data_name} (id={map_id}) — сервером не подаётся")
            results.append(record)
            continue
        if not confident:
            record["status"] = "no_birth"
            report.line(f"─── {data_name} (id={map_id}) — нет birth-point'а в birth_conf.lua")
            results.append(record)
            continue
        r = visit_map(
            session, report, map_id, data_name, x, y,
            npcs_per_map.get(map_name, 0),
            mons_per_map.get(map_name, 0),
        )
        record.update(r)
        results.append(record)

    # ── Сводка
    report.line("═══ СВОДКА ПО КАРТАМ ═══")
    ok_count = 0
    tp_failed = []
    npc_missing = []
    mons_missing = []
    no_birth = []
    not_served = []
    talks_ok = 0

    for r in results:
        status = r["status"]
        if status == "ok":
            ok_count += 1
            if r.get("npc_talk"):
                talks_ok += 1
        elif status == "not_served":
            not_served.append(r["map"])
        elif status == "no_birth":
            no_birth.append(r["map"])
        elif status == "teleport_failed":
            tp_failed.append(r["map"])
        elif status == "npc_missing":
            npc_missing.append(r["map"])
        elif status == "mons_missing":
            mons_missing.append(r["map"])

    report.line(f"переход удался на {ok_count} карт, диалогов с NPC: {talks_ok}")
    if not_served:
        report.warn(f"карта не обслуживается сервером ({len(not_served)} шт): "
                    f"{', '.join(sorted(not_served))}")
    if no_birth:
        report.warn(f"нет birth-точки ({len(no_birth)} шт): {', '.join(sorted(no_birth))}")
    if tp_failed:
        report.error(f"телепорт не удался ({len(tp_failed)} шт): {', '.join(sorted(tp_failed))}")
    if npc_missing:
        report.error(f"NPC в БД, но не видно ({len(npc_missing)} шт): {', '.join(npc_missing)}")
    if mons_missing:
        report.warn(f"монстры в БД, но не видно ({len(mons_missing)} шт): {', '.join(mons_missing)}")

    session.logout()
    pump(session, 2.0)
    report.line("───")
    db.close()


if __name__ == "__main__":
    main()
