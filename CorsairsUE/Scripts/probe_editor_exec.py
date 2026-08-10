"""Минимальная проверка, что -ExecutePythonScript вообще запускается."""

import sys

sys.path.insert(0, "/Users/ivan/code/TalesOfPirate-kimi-garner/CorsairsUE/Scripts")

import unreal

unreal.log("PROBE_EXEC_PYTHON_SCRIPT_STARTED")


class Tick:
    def __init__(self):
        self.count = 0
        self.handle = unreal.register_slate_post_tick_callback(self.on_tick)

    def on_tick(self, _dt):
        self.count += 1
        if self.count == 30:
            unreal.log("PROBE_EXEC_TICK30 ok")
            unreal.unregister_slate_post_tick_callback(self.handle)
            unreal.EditorPythonScripting.set_keep_python_script_alive(False)


probe = Tick()
