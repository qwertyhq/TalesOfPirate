"""Создаёт изолированную playable-копию progress-карты Garner.

Исходная карта никогда не сохраняется. Скрипт владеет только точной целевой
картой и её transaction-соседями, удаляет запечённый screenshot pawn и после
холодной перезагрузки проверяет gameplay-контракт. Повторный запуск даёт то же
состояние целевой карты.

Запускать через ``UnrealEditor-Cmd -run=pythonscript``; ``--source``/``--target``
можно передавать только с путями, объявленными ниже.
"""

import argparse
from pathlib import Path
import posixpath
import sys


SCRIPT_DIR = Path(__file__).resolve().parent
CONTENT_DIR = SCRIPT_DIR.parent / "Content"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from report import RefuseIfEditorOpen, Reporter  # noqa: E402


SOURCE_LEVEL = "/Game/Maps/GarnerSceneProgressCity"
TARGET_LEVEL = "/Game/Maps/GarnerSceneProgressPlay"
BAKED_CHARACTER_LABEL = "SceneProgressCity_Test195126"
# Манекен собран из пяти частей, каждая — отдельный актор: редакторный Python
# не умеет добавлять компоненты к актору уровня (см. spawn_character).
BAKED_CHARACTER_PARTS = 5
BAKED_NPC_PREFIX = "SceneProgressCity_NPC_"
BAKED_NPC_COUNT = 5
PLAYER_START_CLASS = "/Script/Engine.PlayerStart"
# Screenshot-манекен ставится обычным SkeletalMeshActor: игровой
# ACorsairsPlayerCharacter собирает облик через ApplyAppearance, а этот путь в
# Python не выведен (см. spawn_character в place_scene_progress_city.py).
CHARACTER_CLASS = "/Script/Engine.SkeletalMeshActor"
GAME_MODE_CLASS = "/Script/CorsairsGame.CorsairsGameMode"

TRANSACTION_TEMP_SUFFIX = "__TxnTemp"
TRANSACTION_BACKUP_SUFFIX = "__TxnBackup"


def validate_contract(source, target):
    """Проверить границу владения картой до обращения к Unreal."""
    if source == target:
        raise RuntimeError("исходная и целевая карты совпадают")
    if source != SOURCE_LEVEL:
        raise RuntimeError(
            f"исходная карта должна быть ровно {SOURCE_LEVEL}, получено {source!r}"
        )
    if target != TARGET_LEVEL:
        raise RuntimeError(
            f"целевая карта должна быть ровно {TARGET_LEVEL}, получено {target!r}"
        )


def owned_transaction_paths(target):
    """Вернуть только точные временные пути, которыми владеет скрипт."""
    if target != TARGET_LEVEL:
        raise RuntimeError(
            f"transaction владеет только {TARGET_LEVEL}, получено {target!r}"
        )
    return (
        target + TRANSACTION_TEMP_SUFFIX,
        target + TRANSACTION_BACKUP_SUFFIX,
    )


def owned_map_package_file(asset_path):
    """Вернуть файл только для собственного служебного пути карты."""
    temporary, backup = owned_transaction_paths(TARGET_LEVEL)
    if asset_path not in (temporary, backup):
        raise RuntimeError(
            f"отказ удаления не служебной карты транзакции: {asset_path}"
        )
    relative = asset_path.removeprefix("/Game/")
    return CONTENT_DIR.joinpath(*relative.split("/")).with_suffix(".umap")


def delete_owned_map(library, asset_path, unreal_module=None):
    """Удалить exact temp/backup, учитывая молчаливый отказ UE 5.8."""
    package = owned_map_package_file(asset_path)
    library.delete_asset(asset_path)
    if not library.does_asset_exist(asset_path):
        return True

    if package.is_file():
        package.unlink()
    if unreal_module is None:
        import unreal as unreal_module
    registry = unreal_module.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(
        [posixpath.dirname(asset_path)], force_rescan=True
    )
    return not library.does_asset_exist(asset_path)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Подготовить изолированную playable-копию Garner"
    )
    parser.add_argument("--source", default=SOURCE_LEVEL)
    parser.add_argument("--target", default=TARGET_LEVEL)
    return parser.parse_args(argv)


def recover_owned_transaction(library, target, validate_published=None):
    """Восстановить stale-соседей, отказав при неоднозначном состоянии."""
    temporary, backup = owned_transaction_paths(target)
    has_target = library.does_asset_exist(target)
    has_temporary = library.does_asset_exist(temporary)
    has_backup = library.does_asset_exist(backup)

    if has_target and has_backup and has_temporary:
        raise RuntimeError(
            f"неоднозначное transaction state: существуют {target}, "
            f"{temporary} и {backup}"
        )
    if has_target and has_backup:
        if validate_published is None:
            raise RuntimeError(
                f"неоднозначное transaction state: существуют {target} и {backup}"
            )
        validate_published()
        if not delete_owned_map(library, backup):
            raise RuntimeError(f"не удалён подтверждённый backup {backup}")
        has_target = True
        has_temporary = False
        has_backup = False
    if has_backup and not has_target:
        if not library.rename_asset(backup, target):
            raise RuntimeError(f"не восстановлен backup {backup} -> {target}")
        has_target = True
    if has_temporary:
        if not delete_owned_map(library, temporary):
            raise RuntimeError(f"не удалён stale temp {temporary}")
        has_temporary = False
    if library.does_asset_exist(backup):
        raise RuntimeError(f"backup остался после recovery: {backup}")


def _actor_class_path(actor):
    actor_class = actor.get_class()
    get_path_name = getattr(actor_class, "get_path_name", None)
    if not callable(get_path_name):
        raise RuntimeError("actor class не предоставляет get_path_name")
    return get_path_name()


def _actors_with_label(actor_subsystem, label):
    return [
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if actor.get_actor_label().startswith(label)
    ]


def remove_baked_character(report, unreal, actor_subsystem):
    matches = _actors_with_label(actor_subsystem, BAKED_CHARACTER_LABEL)
    if matches and len(matches) != BAKED_CHARACTER_PARTS:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: label {BAKED_CHARACTER_LABEL} найден "
            f"{len(matches)} раз(а), ожидалось {BAKED_CHARACTER_PARTS}"
        )
    if not matches:
        report.line("BAKED CHARACTER: уже удалён")
        return

    for actor in matches:
        class_path = _actor_class_path(actor)
        if class_path != CHARACTER_CLASS:
            raise RuntimeError(
                f"отказ удаления чужого actor: {actor.get_actor_label()} имеет "
                f"класс {class_path}, ожидался {CHARACTER_CLASS}"
            )
        if not actor_subsystem.destroy_actor(actor):
            raise RuntimeError(
                f"не удалён baked actor {actor.get_actor_label()}")
    if _actors_with_label(actor_subsystem, BAKED_CHARACTER_LABEL):
        raise RuntimeError(f"baked actor остался после удаления {BAKED_CHARACTER_LABEL}")
    report.line(
        f"BAKED CHARACTER: удалён screenshot pawn целиком, "
        f"частей={len(matches)}")


def remove_baked_npcs(report, unreal, actor_subsystem):
    """Удалить только пять screenshot-NPC: живых создаёт GameMode."""
    matches = _actors_with_label(actor_subsystem, BAKED_NPC_PREFIX)
    if matches and len(matches) != BAKED_NPC_COUNT:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: prefix {BAKED_NPC_PREFIX} найден "
            f"{len(matches)} раз(а), ожидалось {BAKED_NPC_COUNT}"
        )
    if not matches:
        report.line("BAKED NPC: уже удалены")
        return

    # Сначала проверяем весь exact-набор. Пока проверка не завершена, карта
    # остаётся нетронутой: ошибка в последнем актёре не удалит первые четыре.
    for actor in matches:
        class_path = _actor_class_path(actor)
        if class_path != CHARACTER_CLASS:
            raise RuntimeError(
                f"отказ удаления чужого actor: {actor.get_actor_label()} имеет "
                f"класс {class_path}, ожидался {CHARACTER_CLASS}"
            )

    for actor in matches:
        if not actor_subsystem.destroy_actor(actor):
            raise RuntimeError(
                f"не удалён baked NPC {actor.get_actor_label()}"
            )
    if _actors_with_label(actor_subsystem, BAKED_NPC_PREFIX):
        raise RuntimeError(f"baked NPC остались: {BAKED_NPC_PREFIX}")
    report.line(f"BAKED NPC: удалено {len(matches)} screenshot actors")


def _game_mode_path(world_settings):
    game_mode = world_settings.get_editor_property("default_game_mode")
    if game_mode is None:
        return None
    get_path_name = getattr(game_mode, "get_path_name", None)
    return get_path_name() if callable(get_path_name) else None


def ensure_gameplay_contract(report, unreal, actor_subsystem):
    player_starts = [
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if isinstance(actor, unreal.PlayerStart)
    ]
    if len(player_starts) != 1:
        raise RuntimeError(
            f"НЕВЕРНОЕ_КОЛИЧЕСТВО: PlayerStart={len(player_starts)}, ожидался 1"
        )

    game_mode = unreal.load_class(None, GAME_MODE_CLASS)
    if game_mode is None:
        raise RuntimeError(f"класс режима не найден: {GAME_MODE_CLASS}")
    world = unreal.EditorLevelLibrary.get_editor_world()
    if world is None:
        raise RuntimeError("редактор не вернул текущий world")
    world_settings = world.get_world_settings()
    if world_settings is None:
        raise RuntimeError("world settings не найдены")
    world_settings.set_editor_property("default_game_mode", game_mode)
    if _game_mode_path(world_settings) != GAME_MODE_CLASS:
        raise RuntimeError(
            f"default GameMode не подтверждён: {_game_mode_path(world_settings)!r}"
        )
    report.line("GAMEPLAY CONTRACT: PlayerStart=1 GameMode=CorsairsGameMode")


def verify_target(unreal, actor_subsystem):
    remaining = _actors_with_label(actor_subsystem, BAKED_CHARACTER_LABEL)
    if remaining:
        raise RuntimeError(
            f"readback: baked actor всё ещё существует: {BAKED_CHARACTER_LABEL}"
        )
    remaining_npcs = _actors_with_label(actor_subsystem, BAKED_NPC_PREFIX)
    if remaining_npcs:
        raise RuntimeError(
            f"readback: baked NPC всё ещё существуют: {BAKED_NPC_PREFIX}"
        )
    player_starts = [
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if isinstance(actor, unreal.PlayerStart)
    ]
    if len(player_starts) != 1:
        raise RuntimeError(f"readback: PlayerStart={len(player_starts)}, ожидался 1")
    world = unreal.EditorLevelLibrary.get_editor_world()
    game_mode_path = _game_mode_path(world.get_world_settings())
    if game_mode_path != GAME_MODE_CLASS:
        raise RuntimeError(f"readback: GameMode={game_mode_path!r}")


def _rollback_publish(library, levels, source, target, temporary, backup, had_target):
    if library.does_asset_exist(target):
        if library.does_asset_exist(temporary):
            raise RuntimeError("rollback: target и temp существуют одновременно")
        if not library.rename_asset(target, temporary):
            raise RuntimeError(f"rollback: target не убран {target} -> {temporary}")
    if had_target:
        if (not library.does_asset_exist(backup)
                or not library.rename_asset(backup, target)):
            raise RuntimeError(f"rollback: backup не восстановлен {backup} -> {target}")
    if library.does_asset_exist(temporary):
        if not delete_owned_map(library, temporary):
            raise RuntimeError(f"rollback: temp не удалён {temporary}")
    if not levels.load_level(source):
        raise RuntimeError(f"rollback: source не загрузился {source}")


def publish_transaction(report, library, levels, source, target):
    temporary, backup = owned_transaction_paths(target)
    if not library.does_asset_exist(temporary):
        raise RuntimeError(f"publish: temp отсутствует {temporary}")
    if not levels.load_level(source):
        raise RuntimeError(f"publish: source не загрузился {source}")

    had_target = library.does_asset_exist(target)
    if had_target and not library.rename_asset(target, backup):
        raise RuntimeError(f"publish: target не перемещён в backup {target}")
    if not library.rename_asset(temporary, target):
        if had_target and not library.rename_asset(backup, target):
            raise RuntimeError("publish rollback: old target не восстановлен")
        raise RuntimeError(f"publish: temp не переименован {temporary} -> {target}")

    if not levels.load_level(target):
        _rollback_publish(library, levels, source, target, temporary, backup, had_target)
        raise RuntimeError(f"publish: target не загрузился {target}")
    if had_target and not delete_owned_map(library, backup):
        # Удаление могло физически убрать package, пока Asset Registry ещё видит
        # stale backup. Новый target уже загружен и проверен; откат к, возможно,
        # исчезнувшей копии здесь способен уничтожить единственную рабочую карту.
        raise RuntimeError(
            f"publish: backup не удалён {backup}; подтверждённый target сохранён"
        )
    if (not library.does_asset_exist(target)
            or library.does_asset_exist(temporary)
            or library.does_asset_exist(backup)):
        raise RuntimeError("publish: неверное финальное asset state")
    report.line(f"PLAYABLE MAP PUBLISHED: {target}")


def run(report, unreal, source=SOURCE_LEVEL, target=TARGET_LEVEL):
    validate_contract(source, target)
    library = unreal.EditorAssetLibrary
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    temporary, _backup = owned_transaction_paths(target)

    def validate_recovery_target():
        if not levels.load_level(target):
            raise RuntimeError(f"recovery: target не загрузился {target}")
        verify_target(unreal, actors)

    recover_owned_transaction(
        library,
        target,
        validate_recovery_target,
    )
    if not library.does_asset_exist(source):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MAP: source={source}")
    if not levels.load_level(source):
        raise RuntimeError(f"ОТСУТСТВУЕТ_MAP: source не загружен {source}")

    duplicate = library.duplicate_asset(source, temporary)
    if duplicate is None or not library.does_asset_exist(temporary):
        raise RuntimeError(f"не создан temp map {source} -> {temporary}")
    if not levels.load_level(temporary):
        raise RuntimeError(f"temp map не загружен {temporary}")
    try:
        remove_baked_character(report, unreal, actors)
        remove_baked_npcs(report, unreal, actors)
        ensure_gameplay_contract(report, unreal, actors)
        if not levels.save_current_level():
            raise RuntimeError(f"temp map не сохранён {temporary}")
        if not levels.load_level(temporary):
            raise RuntimeError(f"temp map readback не загружен {temporary}")
        verify_target(unreal, actors)
        report.line("READBACK: baked=0 PlayerStart=1 GameMode=CorsairsGameMode")
        publish_transaction(report, library, levels, source, target)
        if not levels.load_level(target):
            raise RuntimeError(f"published target не загружен {target}")
        verify_target(unreal, actors)
        report.line(f"УСПЕХ: playable map готова {target}")
    except Exception:
        if library.does_asset_exist(temporary):
            if not delete_owned_map(library, temporary):
                raise RuntimeError(f"abort: temp не удалён {temporary}")
        if library.does_asset_exist(_backup):
            raise RuntimeError(f"abort: backup остался {_backup}")
        if not levels.load_level(source):
            raise RuntimeError(f"abort: source не загружен {source}")
        raise


def main(argv=None):
    report = Reporter("prepare_playable_scene_progress")
    try:
        if RefuseIfEditorOpen(report):
            raise RuntimeError("закройте графический UnrealEditor перед commandlet")
        args = parse_args(sys.argv[1:] if argv is None else argv)
        validate_contract(args.source, args.target)
        import unreal
        run(report, unreal, args.source, args.target)
    finally:
        report.close()


if __name__ == "__main__":
    main()
