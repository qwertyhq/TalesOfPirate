"""Отчёт скриптов редактора в файл.

`unreal.log` в режиме `-run=pythonscript` не доходит до stdout: коммандлет
печатает `Python script executed successfully` независимо от того, что скрипт
насчитал и что вывел. Молчание при этом неотличимо от успеха — а именно так
выглядела бы расстановка, не поставившая ни одного объекта.

Поэтому каждый скрипт пишет отчёт в файл рядом с собой и дублирует строки в
`unreal.log` — вдруг лог всё же виден в другом режиме запуска.

    from report import Reporter

    report = Reporter("place_objects")
    report.line("РАССТАВЛЕНО инстансов=12345")
    report.warn("ПРОПУЩЕНО объектов=783")
    report.close()
"""

import os
import traceback

try:
    import unreal
except ImportError:      # вне редактора — для отладки самого модуля
    unreal = None

REPORT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports")


class Reporter:
    def __init__(self, name):
        os.makedirs(REPORT_DIR, exist_ok=True)
        self.path = os.path.join(REPORT_DIR, f"{name}.txt")
        self._handle = open(self.path, "w", encoding="utf-8")

    def line(self, text):
        self._handle.write(text + "\n")
        self._handle.flush()
        if unreal:
            unreal.log(text)

    def warn(self, text):
        self.line("ПРЕДУПРЕЖДЕНИЕ " + text)
        if unreal:
            unreal.log_warning(text)

    def error(self, text):
        self.line("ОШИБКА " + text)
        if unreal:
            unreal.log_error(text)

    def exception(self, exc):
        """Пишет исключение целиком.

        Без этого упавший скрипт неотличим от отработавшего: коммандлет
        сообщает об успехе в обоих случаях.
        """
        self.line("ИСКЛЮЧЕНИЕ " + repr(exc))
        self._handle.write(traceback.format_exc())
        self._handle.flush()

    def close(self):
        self._handle.close()
