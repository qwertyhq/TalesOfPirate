"""Проверяет сетевой слой UE против живых серверов.

    UnrealEditor-Cmd CorsairsUE.uproject -run=pythonscript \\
        -script="Scripts/check_login.py [хост] [порт] [учётная-запись] [пароль]"

По умолчанию: 127.0.0.1 1973 admin admin.
Отчёт: `Scripts/reports/check_login.txt`.

Проходит вход целиком силами самого UE: подключение к GateServer, CM_LOGIN,
второй пароль при необходимости, CM_BGNPLAY и ожидание MC_ENTERMAP от
GameServer. Тем самым проверяется, что протокол из sources/Libraries работает
внутри движка, а не только в консольном клиенте на .NET.

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
TIMEOUT_SECONDS = 40.0

STAGE_NAMES = {
    unreal.CorsairsLoginStage.IDLE: "не начат",
    unreal.CorsairsLoginStage.CONNECTING: "подключение",
    unreal.CorsairsLoginStage.AUTHENTICATING: "проверка учётной записи",
    unreal.CorsairsLoginStage.SELECTING_CHA: "выбор персонажа",
    unreal.CorsairsLoginStage.ENTERING_WORLD: "вход в мир",
    unreal.CorsairsLoginStage.IN_WORLD: "в мире",
    unreal.CorsairsLoginStage.FAILED: "ошибка",
}


def stage_name(stage):
    return STAGE_NAMES.get(stage, str(stage))


def pump_until(report, session, wanted, timeout=TIMEOUT_SECONDS):
    """Качает соединение, пока стадия не окажется в `wanted`.

    Возвращает достигнутую стадию либо None по истечении времени.
    """
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        session.poll()
        stage = session.get_stage()
        if stage != last:
            report.line(f"  стадия: {stage_name(stage)}")
            last = stage
        if stage in wanted:
            return stage
        time.sleep(POLL_INTERVAL)
    return None


def main(report):
    args = sys.argv[1:]
    host = args[0] if len(args) > 0 else "127.0.0.1"
    port = int(args[1]) if len(args) > 1 else 1973
    account = args[2] if len(args) > 2 else "admin"
    password = args[3] if len(args) > 3 else "admin"

    report.line(f"вход {account} на {host}:{port}")

    # Объект держится в переменной до конца прогона: без ссылки сборщик мусора
    # UE снесёт сессию вместе с открытым сокетом.
    session = unreal.new_object(unreal.CorsairsSession)
    session.login(host, port, account, password)

    stage = pump_until(
        report, session,
        {unreal.CorsairsLoginStage.SELECTING_CHA, unreal.CorsairsLoginStage.FAILED})

    if stage is None:
        report.error("ПРОВАЛ: ответа на вход не дождались")
        return
    if stage == unreal.CorsairsLoginStage.FAILED:
        report.error("ПРОВАЛ: вход отклонён")
        return

    characters = session.get_characters()
    report.line(f"ВХОД ВЫПОЛНЕН, персонажей: {len(characters)}")

    slot = -1
    for index, cha in enumerate(characters):
        if cha.valid:
            report.line(f"  слот {index}: {cha.name}, уровень {cha.level}, тип {cha.type_id}")
            if slot < 0:
                slot = index

    if slot < 0:
        report.warn("персонажей нет — вход в мир не проверяется; "
                    "создать персонажа можно консольным клиентом на .NET")
        report.line("УСПЕХ: вход в учётную запись работает")
        return

    session.enter_world(slot)
    stage = pump_until(
        report, session,
        {unreal.CorsairsLoginStage.IN_WORLD, unreal.CorsairsLoginStage.FAILED})

    if stage == unreal.CorsairsLoginStage.IN_WORLD:
        spawn = session.get_spawn_position()
        report.line(f"В МИРЕ: карта {session.get_map_name()}, "
                    f"позиция ({spawn.x}, {spawn.y}), worldId {session.get_world_id()}")

        # Движение проверяется отдельно: вход в мир его не затрагивает, а
        # сервер принимает путь, а не мгновенное положение — и отвергает
        # перемещение через непроходимые клетки.
        target = unreal.IntPoint(spawn.x + 200, spawn.y)
        sent = session.send_move_path([spawn, target])
        report.line(f"путь отправлен: {sent}")

        if not sent:
            report.error("ПРОВАЛ: путь движения не отправился")
            return

        # Ответ приходит командой NOTIACTION; сюда она не разбирается, но
        # разрыв соединения означал бы отвергнутый пакет.
        for _ in range(40):
            session.poll()
            if session.get_stage() != unreal.CorsairsLoginStage.IN_WORLD:
                report.error("ПРОВАЛ: сервер разорвал связь после команды движения")
                return
            time.sleep(POLL_INTERVAL)

        # Сервер присылает то, что попало в поле зрения: NPC, монстров и
        # других игроков. Именно так населяется мир — запекать их в уровень
        # не нужно.
        # Список ведёт сама сессия: делегат-свойство при обращении из Python
        # отдаёт копию, и подписка на неё теряется.
        for _ in range(120):
            session.poll()
            time.sleep(POLL_INTERVAL)
        seen = session.get_visible_actors()
        report.line(f"ПЕРСОНАЖЕЙ В ПОЛЕ ЗРЕНИЯ: {len(seen)}")
        cmds = session.get_received_commands()
        top = sorted(cmds.items(), key=lambda kv: -kv[1])[:8]
        report.line(f"команд от сервера: {sum(cmds.values())} всего, "
                    f"{len(cmds)} различных")
        report.line(f"  частые (номер: сколько): {top}")
        for a in seen[:6]:
            report.line(f"  {a.name or '(без имени)'} — тип {a.type_id}, "
                        f"позиция ({a.position.x}, {a.position.y})")

        report.line("УСПЕХ: клиент в мире, команда движения принята сервером")
    elif stage == unreal.CorsairsLoginStage.FAILED:
        report.error("ПРОВАЛ: вход в мир отклонён")
    else:
        report.error("ПРОВАЛ: входа в мир не дождались")

    session.logout()


report = Reporter("check_login")
try:
    main(report)
except Exception as exc:                        # noqa: BLE001
    report.exception(exc)
finally:
    report.close()
