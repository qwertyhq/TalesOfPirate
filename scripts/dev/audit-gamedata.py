#!/usr/bin/env python3
"""Сплошная проверка согласованности игровых данных.

    python3 scripts/dev/audit-gamedata.py [путь-к-базе]

По умолчанию читается `databases/gamedata.sqlite`.

Зачем отдельно от бота. Бот проверяет, работает ли механика, и делает это на
одном-двух примерах: один монстр, один NPC, одно умение. Вопросы вида «у всех
ли умений заполнены уровни» или «нет ли предмета, выпадающего из ряда себе
подобных» так не решаются — их надо задавать всей базе разом. Здесь ровно это
и делается: полный обход без сервера и без сети.

Что считается находкой. Битая ссылка — всегда дефект: значение указывает на
запись, которой нет. Числовой выброс — не дефект, а подозрение: «правильного»
значения не существует, но предмет, чьи характеристики в разы отличаются от
одноуровневых собратьев, чаще всего опечатка в данных. Поэтому выбросы
выводятся отдельным разделом и с оговоркой.
"""

import os
import sqlite3
import statistics
import sys

DEFAULT_DB = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "..", "databases", "gamedata.sqlite")

# Во сколько раз характеристика должна превысить медиану одноуровневых, чтобы
# считаться подозрительной. Порог намеренно грубый: цель — поймать опечатку в
# разряде, а не спорить о балансе.
OUTLIER_FACTOR = 8.0

# Меньше этого числа образцов на уровень медиана ничего не значит.
MIN_SAMPLE = 5


class Report:
    """Копит находки и печатает их сгруппированно."""

    def __init__(self):
        self.sections = []
        self.defects = 0
        self.suspicions = 0

    def defect(self, title, rows):
        if rows:
            self.defects += len(rows)
            self.sections.append(("ДЕФЕКТ", title, rows))

    def suspicion(self, title, rows):
        if rows:
            self.suspicions += len(rows)
            self.sections.append(("подозрение", title, rows))

    def note(self, title, rows):
        if rows:
            self.sections.append(("сведения", title, rows))

    def render(self):
        for kind, title, rows in self.sections:
            print(f"\n[{kind}] {title} — {len(rows)}")
            for row in rows[:15]:
                print(f"    {row}")
            if len(rows) > 15:
                print(f"    … ещё {len(rows) - 15}")
        print()
        print(f"Дефектов: {self.defects}, подозрений: {self.suspicions}")


def as_number(value):
    """Приводит значение к числу, чем бы оно ни было записано.

    Часть числовых столбцов хранится текстом: таблицы пришли из исходных `.txt`,
    где типов у столбцов не было, и при переносе тип не восстанавливали.
    """
    if value is None:
        return 0.0
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip()
    if not text:
        return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def table_exists(db, name):
    row = db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone()
    return row is not None


def ids_of(db, table):
    return {r[0] for r in db.execute(f"SELECT id FROM {table}")}


def check_skill_requirements(db, report):
    """Умение не может требовать умения, которого нет."""
    skills = ids_of(db, "skills")
    broken = []
    for sid, name, premiss in db.execute(
            "SELECT id, name, premiss_skill FROM skills"):
        if premiss in (None, "", "-1", -1, 0):
            continue
        # Поле хранит либо номер, либо пару «номер,уровень».
        head = str(premiss).split(",")[0].strip()
        if not head.lstrip("-").isdigit():
            continue
        need = int(head)
        if need > 0 and need not in skills:
            broken.append(f"умение {sid} «{name}» требует несуществующего {need}")
    report.defect("умения ссылаются на несуществующее предварительное", broken)


def check_skill_effects(db, report):
    """Умение без сценария эффекта не наносит ничего — но не всякому он нужен.

    Судим по собратьям того же типа: если у большинства умений типа сценарий
    заполнен, отсутствие у остальных — пробел в данных. Там, где его нет ни у
    кого (пассивные умения), это устройство типа, а не дефект.
    """
    by_type = {}
    for sid, name, effect, stype in db.execute(
            "SELECT id, name, effect, type FROM skills"):
        empty = effect is None or str(effect).strip() in ("", "0", "-1")
        by_type.setdefault(stype, []).append((sid, name, empty))

    gaps = []
    for stype, entries in sorted(by_type.items(), key=lambda kv: str(kv[0])):
        blank = [e for e in entries if e[2]]
        if not blank or len(blank) == len(entries):
            # Не заполнено ни у кого — так устроен тип; заполнено у всех —
            # придраться не к чему.
            continue
        for sid, name, _ in blank:
            gaps.append(f"умение {sid} «{name}» типа {stype} без сценария "
                        f"(у {len(entries) - len(blank)} собратьев он есть)")
    report.defect("умения без сценария эффекта на фоне собратьев", gaps)


def check_monster_rewards(db, report):
    """Монстр без опыта — обычно недозаполненная запись."""
    if not table_exists(db, "characters"):
        return
    rows = [(cid, name, lv, exp) for cid, name, lv, exp in db.execute(
        "SELECT id, name, lv, mobexp FROM characters WHERE ctrl_type = 5")
        if as_number(lv) > 0]
    silent = [f"монстр {cid} «{name}» уровня {lv}"
              for cid, name, lv, exp in rows if as_number(exp) <= 0]

    if len(silent) == len(rows):
        # Поле пусто у всех до единого — значит опыт считается формулой от
        # уровня, а не берётся из записи. Проверено на живом сервере: бот
        # получает опыт за удар по монстру с нулевым mobexp.
        report.note("награда опытом в записях монстров",
                    [f"поле не заполнено ни у одного из {len(rows)} — "
                     "опыт начисляется формулой, а не из данных"])
    else:
        report.suspicion("монстры без награды опытом на фоне остальных", silent)


def check_monster_placement(db, report):
    """Монстр из списка размещения обязан существовать в таблице персонажей."""
    if not table_exists(db, "monster_list"):
        return
    # Имена сравниваются без краевых пробелов: они пришли из табличных .txt,
    # где хвостовой пробел — след разделителя, а не часть имени.
    known = {str(name).strip() for (name,) in db.execute("SELECT name FROM characters")}
    missing = []
    for name, mapname in db.execute(
            "SELECT name, map_name FROM monster_list"):
        clean = str(name or "").strip()
        if clean and clean not in known and clean != "Monster Name":
            missing.append(f"«{clean}» размещён на «{str(mapname).strip()}», но записи нет")
    report.defect("размещённые монстры без записи персонажа", missing)


def check_map_names(db, report):
    """Карта размещения обязана быть известна таблице карт."""
    if not (table_exists(db, "monster_list") and table_exists(db, "maps")):
        return
    known = {str(n).strip() for (n,) in db.execute("SELECT name FROM maps")}
    known |= {str(n).strip() for (n,) in db.execute("SELECT data_name FROM maps")}
    missing = sorted({
        str(m).strip() for (m,) in db.execute("SELECT DISTINCT map_name FROM monster_list")
        if m and str(m).strip() not in known and str(m).strip() != "Region"
    })
    report.defect("карты размещения, неизвестные таблице карт",
                  [f"«{m}»" for m in missing])


def check_item_levels(db, report):
    """Требование уровня выше предельного делает вещь недостижимой."""
    max_level = as_number(db.execute("SELECT MAX(level) FROM levels").fetchone()[0])
    if not max_level:
        return
    unreachable = []
    for iid, name, need in db.execute("SELECT id, name, need_lv FROM items"):
        if as_number(need) > max_level:
            unreachable.append(
                f"предмет {iid} «{name}» требует уровень {need} > {max_level}")
    report.defect(f"предметы с требованием выше предельного уровня {max_level}",
                  unreachable)


def check_level_curve(db, report):
    """Опыт по уровням обязан расти: провал означает, что уровень не берётся."""
    rows = list(db.execute("SELECT level, exp FROM levels ORDER BY level"))
    broken = []
    previous = None
    for level, exp in rows:
        exp = as_number(exp)
        if previous is not None and exp < previous:
            broken.append(f"уровень {level}: опыт {exp} меньше предыдущего {previous}")
        previous = exp
    report.defect("опыт по уровням не растёт", broken)


def check_item_outliers(db, report):
    """Ищет вещи, выпадающие из ряда себе подобных по одному уровню.

    Сравнение внутри группы «уровень + тип»: у предметов разных типов
    характеристики несопоставимы по величине, и общий ряд дал бы шум.
    """
    columns = ["mx_atk_valu", "def_valu", "mx_hp_valu"]
    groups = {}
    query = "SELECT id, name, need_lv, type, " + ", ".join(columns) + " FROM items"
    for row in db.execute(query):
        iid, name, need_lv, itype = row[0], row[1], row[2] or 0, row[3]
        groups.setdefault((need_lv, itype), []).append((iid, name, row[4:]))

    findings = []
    for (need_lv, itype), entries in groups.items():
        if len(entries) < MIN_SAMPLE:
            continue
        for index, column in enumerate(columns):
            # Числовые поля местами хранятся текстом: часть таблиц пришла из
            # исходных .txt, где типа у столбца не было вовсе.
            values = [as_number(e[2][index]) for e in entries]
            positive = [v for v in values if v > 0]
            if len(positive) < MIN_SAMPLE:
                continue
            median = statistics.median(positive)
            if median <= 0:
                continue
            for iid, name, vals in entries:
                value = as_number(vals[index])
                if value > median * OUTLIER_FACTOR:
                    findings.append(
                        f"{column}={value} у «{name}» ({iid}), "
                        f"медиана уровня {need_lv} типа {itype} = {median:g}")
    report.suspicion("характеристики, резко выпадающие из ряда одноуровневых",
                     findings)


def check_job_equipment(db, report):
    """Класс без разрешённой экипировки не может одеться."""
    if not table_exists(db, "job_equip"):
        return
    rows = list(db.execute("SELECT id, job, items FROM job_equip"))
    empty = [f"класс «{job}» (запись {jid}) без списка вещей"
             for jid, job, items in rows
             if not items or not str(items).strip()]
    report.defect("классы без разрешённой экипировки", empty)
    report.note("записей о доступной классам экипировке",
                [f"всего {len(rows)}"])


def check_skill_jobs(db, report):
    """Умение, привязанное к несуществующему классу, недоступно никому."""
    jobs = set()
    for (job,) in db.execute("SELECT job FROM job_equip"):
        if job is not None:
            jobs.add(str(job).strip())
    if not jobs:
        return
    orphan = []
    for sid, name, job_select in db.execute(
            "SELECT id, name, job_select FROM skills"):
        if not job_select:
            continue
        head = str(job_select).split(",")[0].strip()
        if head in ("-1", ""):
            continue
        if head not in jobs and not head.lstrip("-").isdigit():
            orphan.append(f"умение {sid} «{name}» для класса «{head}»")
    report.defect("умения для несуществующего класса", orphan)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DB
    if not os.path.exists(path):
        print(f"нет базы: {path}", file=sys.stderr)
        return 2

    db = sqlite3.connect(path)
    report = Report()

    print(f"Аудит {os.path.relpath(path)}")
    counts = []
    for table in ("items", "skills", "characters", "levels", "maps", "monster_list"):
        if table_exists(db, table):
            n = db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            counts.append(f"{table}: {n}")
    print("  " + ", ".join(counts))

    check_skill_requirements(db, report)
    check_skill_effects(db, report)
    check_skill_jobs(db, report)
    check_monster_rewards(db, report)
    check_monster_placement(db, report)
    check_map_names(db, report)
    check_item_levels(db, report)
    check_level_curve(db, report)
    check_item_outliers(db, report)
    check_job_equipment(db, report)

    report.render()
    return 1 if report.defects else 0


if __name__ == "__main__":
    sys.exit(main())
