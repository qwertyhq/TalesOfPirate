"""Проверяет боевой слой клиента на Unreal против живых серверов.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_combat.py [хост] [порт] [учётная-запись] [пароль]"

По умолчанию: 127.0.0.1 1973 bot admin.
Отчёт: `Scripts/reports/check_combat.txt`.

Те же механики, что прогоняет консольный бот, но силами самого движка. Смысл
не в дублировании: бот доказывает, что механика работает на сервере, а этот
прогон — что её повторяет наш клиент. Расхождение между ними означало бы
ошибку переноса, и найти её иначе нельзя — сервер в обоих случаях отвечает
одинаково молчаливо.

Учётная запись по умолчанию `bot`, а не `admin`: под одной записью сервер не
пускает дважды, и прогон конфликтовал бы с открытым игровым клиентом.

Приём качается вручную: в коммандлете нет игрового цикла, и Tick сетевого
объекта не вызывается.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                   # noqa: E402
from report import Reporter                     # noqa: E402

POLL_INTERVAL = 0.05

# Типы сущностей (EChaCtrlType): NPC и монстры приходят одним сообщением и
# различаются только этим полем.
CTRL_NPC = 2
CTRL_MONS = 5

# Слот правой руки (enumEQUIP_RHAND). Оружие держат руки, и без него сервер
# подтверждает удар, но не разыгрывает его.
EQUIP_RHAND = 9

# Умения обычной атаки, по убыванию неприхотливости: «Dual Implosion» и
# «Greatsword» не требуют определённого оружия, «Sword» требует меч или нож.
ATTACK_SKILLS = [26, 29, 27, 28]

# Стартовый нож. Он лежит в сумке нового персонажа.
WEAPON_ITEM = 8

ATTR_HP = 1
ATTR_GOLD = 8

# Место охоты на карте garner: клетка, где по monster_list живут монстры
# первого уровня. Город бой запрещает, за его пределами — нет.
HUNT_X = 2092
HUNT_Y = 2654

# Карта для проверки перехода. Обязана обслуживаться этим GameServer: список
# его карт виден в журнале при старте.
HOME_MAP = "garner"
HOME_X = 2246
HOME_Y = 2704

OTHER_MAP = "magicsea"
OTHER_X = 1350
OTHER_Y = 550


def pump(session, seconds):
    """Качает приём заданное время, отдавая сообщения обработчику."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        session.poll()
        time.sleep(POLL_INTERVAL)


def pump_until_stage(session, wanted, timeout=40.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        session.poll()
        stage = session.get_stage()
        if stage in wanted:
            return stage
        time.sleep(POLL_INTERVAL)
    return None


def actors_of(session, ctrl):
    return [a for a in session.get_visible_actors() if a.ctrl_type == ctrl]


def by_distance(session, ctrl):
    spawn = session.get_spawn_position()
    return sorted(actors_of(session, ctrl),
                  key=lambda a: (a.position.x - spawn.x) ** 2 + (a.position.y - spawn.y) ** 2)


def nearest(session, ctrl):
    """Ближайшая сущность нужного вида.

    Дальняя цель упёрлась бы в отказ по дальности и проверяла бы не то.
    """
    spawn = session.get_spawn_position()
    candidates = actors_of(session, ctrl)
    if not candidates:
        return None
    return min(candidates,
               key=lambda a: (a.position.x - spawn.x) ** 2 + (a.position.y - spawn.y) ** 2)


def equip_weapon(report, session):
    """Надевает нож, если он лежит в сумке.

    Без оружия сервер отвечает на удар подтверждением, но по цели не приходит
    ничего. Отличить это от поломки боя по молчанию невозможно.
    """
    kitbag = session.get_kitbag()
    for grid, item in kitbag.items():
        if item == WEAPON_ITEM:
            session.equip_item(grid, EQUIP_RHAND)
            pump(session, 3.0)
            report.line(f"  оружие {item} из ячейки {grid} надето")
            return True
    report.line("  оружия в сумке нет — возможно, уже надето")
    return False


def hp_of(session, world_id):
    for actor in session.get_visible_actors():
        if actor.world_id == world_id:
            return actor.hp
    return 0


def main(report):
    args = sys.argv[1:]
    host = args[0] if len(args) > 0 else "127.0.0.1"
    port = int(args[1]) if len(args) > 1 else 1973
    account = args[2] if len(args) > 2 else "bot"
    password = args[3] if len(args) > 3 else "admin"

    # Ссылка держится до конца прогона: без неё сборщик мусора UE снесёт
    # сессию вместе с открытым сокетом.
    session = unreal.new_object(unreal.CorsairsSession)
    session.login(host, port, account, password)

    stage = pump_until_stage(
        session,
        {unreal.CorsairsLoginStage.SELECTING_CHA, unreal.CorsairsLoginStage.FAILED})
    if stage != unreal.CorsairsLoginStage.SELECTING_CHA:
        report.error("ПРОВАЛ: вход в учётную запись не прошёл")
        return

    slot = next((i for i, c in enumerate(session.get_characters()) if c.valid), -1)
    if slot < 0:
        report.error("ПРОВАЛ: у записи нет персонажей — создайте их консольным клиентом")
        return

    session.enter_world(slot)
    stage = pump_until_stage(
        session,
        {unreal.CorsairsLoginStage.IN_WORLD, unreal.CorsairsLoginStage.FAILED})
    if stage != unreal.CorsairsLoginStage.IN_WORLD:
        report.error("ПРОВАЛ: вход в мир не прошёл")
        return

    # Персонаж остаётся там, куда его увёл прошлый прогон. Возвращаем его на
    # обжитую карту, иначе не найдётся ни NPC, ни знакомых монстров.
    if session.get_map_name() != HOME_MAP:
        session.say(f"&move {HOME_X},{HOME_Y},{HOME_MAP}")
        deadline = time.time() + 25.0
        while time.time() < deadline and session.get_map_name() != HOME_MAP:
            session.poll()
            time.sleep(POLL_INTERVAL)
        report.line(f"возврат на {session.get_map_name()}")

    spawn = session.get_spawn_position()
    report.line(f"В МИРЕ: карта {session.get_map_name()}, позиция ({spawn.x}, {spawn.y})")

    # Даём серверу разослать окружение: сущности приходят по мере обхода поля
    # зрения, а не все сразу.
    pump(session, 6.0)
    seen = session.get_visible_actors()
    report.line(f"в поле зрения {len(seen)}: "
                f"NPC {len(actors_of(session, CTRL_NPC))}, "
                f"монстров {len(actors_of(session, CTRL_MONS))}")

    passed = []
    failed = []

    # ── разговор с NPC ──────────────────────────────────────────────────
    npc = nearest(session, CTRL_NPC)
    if npc is None:
        report.warn("рядом нет NPC — разговор не проверяется")
    else:
        # Подойти вплотную: сервер ищет NPC вокруг персонажа, а не по полю
        # зрения. Телепорт GM-командой быстрее, чем ждать проход по пути.
        session.say(f"&move {npc.position.x // 100},{npc.position.y // 100}")
        pump(session, 3.0)
        before = dict(session.get_received_commands())
        session.talk_to_npc(npc.world_id)
        pump(session, 5.0)
        after = session.get_received_commands()
        answered = any(after.get(cmd, 0) > before.get(cmd, 0) for cmd in after)
        if answered:
            passed.append(f"разговор с «{npc.name}»")
        else:
            failed.append(f"NPC «{npc.name}» не ответил")

    # ── бой ─────────────────────────────────────────────────────────────
    # Уходим за город к месту, где по данным игры водятся монстры первого
    # уровня: внутри города сервер запрещает бой признаком зоны.
    session.say(f"&move {HUNT_X},{HUNT_Y}")
    pump(session, 5.0)
    equip_weapon(report, session)
    session.say("&dev on")
    pump(session, 3.0)

    mons = nearest(session, CTRL_MONS)
    if mons is None:
        session.say("&summon 96")
        pump(session, 6.0)
        mons = nearest(session, CTRL_MONS)

    if mons is None:
        report.warn("монстра не нашлось — бой не проверяется")
    else:
        hp_before = hp_of(session, mons.world_id)
        struck = False
        for skill_id in ATTACK_SKILLS:
            session.say(f"&skill {skill_id},1")
            pump(session, 2.0)
            if session.use_skill_on(skill_id, mons.world_id):
                pump(session, 8.0)
                if hp_of(session, mons.world_id) < hp_before:
                    passed.append(f"удар умением {skill_id} по «{mons.name}»: "
                                  f"здоровье {hp_before} → {hp_of(session, mons.world_id)}")
                    struck = True
                    break
        if not struck:
            failed.append(f"ни одно умение не сняло здоровья с «{mons.name}»")

    session.say("&dev off")
    pump(session, 2.0)

    # ── торговля ────────────────────────────────────────────────────────
    session.say("&make 641,1")
    pump(session, 4.0)
    goods = next((g for g, i in session.get_kitbag().items() if i == 641), None)
    # Перебираем нескольких: не всякий NPC торгует, и отказ одного ничего не
    # говорит о механике сделки.
    traders = by_distance(session, CTRL_NPC)[:4]
    sold = False
    if goods is None or not traders:
        report.warn("продавать нечего или некому — торговля не проверяется")
    for trader in ([] if goods is None else traders):
        if sold:
            break
        # Подходим и открываем лавку: сделка ссылается на начатый разговор.
        session.say(f"&move {trader.position.x // 100},{trader.position.y // 100}")
        pump(session, 3.0)
        session.talk_to_npc(trader.world_id)
        pump(session, 2.0)
        session.open_npc_page(trader.world_id, 1, 0)
        pump(session, 3.0)

        gold_before = session.get_attributes().get(ATTR_GOLD, -1)
        session.sell_item_to_npc(trader.world_id, goods, 1)
        pump(session, 5.0)
        gold_after = session.get_attributes().get(ATTR_GOLD, -1)

        # Судим по деньгам: сумка обновляется отдельным сообщением, а приход
        # монет приходит вместе с характеристиками.
        if gold_before >= 0 and gold_after > gold_before:
            passed.append(f"вещь продана «{trader.name}»: "
                          f"денег {gold_before} → {gold_after}")
            sold = True

    if goods is not None and traders and not sold:
        failed.append("ни один NPC не купил вещь")

    # ── переход между картами ───────────────────────────────────────────
    # Соединение при этом не рвётся: Gate сохраняет канал и переводит клиента
    # на сервер целевой карты, откуда приходит новый вход в карту. Клиенту
    # остаётся дождаться его — переподключение было бы ошибкой.
    before_map = session.get_map_name()
    session.say(f"&move {OTHER_X},{OTHER_Y},{OTHER_MAP}")
    deadline = time.time() + 25.0
    while time.time() < deadline and session.get_map_name() == before_map:
        session.poll()
        time.sleep(POLL_INTERVAL)

    if session.get_map_name() == OTHER_MAP:
        passed.append(f"переход «{before_map}» → «{session.get_map_name()}»")
    else:
        failed.append(f"остались на «{session.get_map_name()}» вместо «{OTHER_MAP}»")

    session.logout()

    report.line("───")
    for line in passed:
        report.line(f"  ПРОЙДЕНО  {line}")
    for line in failed:
        report.error(f"  ПРОВАЛ    {line}")
    report.line(f"пройдено {len(passed)}, провалов {len(failed)}")
    if failed:
        report.error("ПРОВАЛ: клиент повторяет не все механики")


report = Reporter("check_combat")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
