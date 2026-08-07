# Unreal Visual Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Unreal client render the authoritative five-part player appearance, the declared original camera profile, and Garner materials without runtime fallback.

**Architecture:** Preserve the full protocol look in `CorsairsNet`, resolve it through one generated catalog, and render it through a stable invisible animation driver with five leader-pose followers. Keep camera math in a pure helper. Fix only the confirmed material usage failure through deterministic editor checker/fixer scripts; do not replace imported material graphs.

**Tech Stack:** Unreal Engine 5.8.1 C++, UE Automation Tests, Python 3 `unittest`, Unreal Python Editor API, SQLite.

## Global Constraints

- Work only in `/Users/ivan/code/TalesOfPirate/codex-ue-parity` on branch `qwertyhq/codex-ue-parity`.
- The approved design is `docs/plans/2026-08-07-ue-visual-parity-design.md`.
- Keep `Client/user/system.ini` and the parent worktree untouched.
- Follow TDD: demonstrate each new check failing before the implementation that makes it pass.
- A human character uses one stable invisible animation driver and exactly five visible leader-pose followers in protocol slots 0 through 4.
- Item identifiers resolve through `items.module_<model + 1>`; never derive an asset name from the item identifier.
- The declared camera profile is `3500 cm` horizontal, `5000 cm` vertical, `32°` vertical FOV, initial UE yaw `+90°`, and target height `100 cm` above the feet. Do not reproduce the original normalization bug.
- At 16:9, Unreal horizontal FOV must be `54.0222°` with `AspectRatio_MaintainYFOV`.
- Preserve existing imported material instances and parents. Use material usage overrides; do not introduce a replacement material graph.
- Translucent instanced components preserve translucency and set `bDisallowNanite`; do not coerce them to opaque or masked.
- Four-layer terrain blending, new PBR art, HUD, packaging, network movement/combat, map travel, and rectangular height maps remain out of scope.
- `CorsairsUEEditor Mac Development` must build after every C++ task.
- No commit may contain generated `__pycache__`, `Scripts/reports`, `Intermediate`, `Saved`, `Binaries`, `Content`, or `Client/user/system.ini`.

---

### Task 1: Generate the authoritative appearance catalog

**Files:**
- Create: `CorsairsUE/Scripts/tests/__init__.py`
- Create: `CorsairsUE/Scripts/tests/test_build_character_map.py`
- Modify: `CorsairsUE/Scripts/build_character_map.py`
- Modify: `CorsairsUE/Data/character_map.json`

**Interfaces:**
- Consumes: `databases/gamedata.sqlite`, tables `characters` and `items`.
- Produces: `build_catalog(db_path: str) -> dict` and deterministic JSON with top-level `characters` and `items`.
- Produces per character: `modalType`, `modelId`, `moduleIndex`, `driverMesh`, `animation`, `defaultItemIds`, `staticMesh`.
- Produces per item: `meshesByModule`, keyed by strings `"1"` through `"4"`.

- [ ] **Step 1: Write the failing generator tests**

Create `CorsairsUE/Scripts/tests/__init__.py` as an empty package marker and create this test module:

```python
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "CorsairsUE" / "Scripts"))

import build_character_map  # noqa: E402


class CharacterMapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = build_character_map.build_catalog(
            str(ROOT / "databases" / "gamedata.sqlite"))

    def test_lambert_has_five_default_items_and_driver(self):
        lambert = self.catalog["characters"]["1"]
        self.assertEqual([2000, 255, 464, 640, 816],
                         lambert["defaultItemIds"])
        self.assertEqual(1, lambert["moduleIndex"])
        self.assertEqual(
            "/Game/Animations/0000/SkeletalMeshes/0000",
            lambert["driverMesh"])
        self.assertEqual(
            "/Game/Animations/0000/SkeletalMeshes/0000_Anim",
            lambert["animation"])

    def test_items_use_model_specific_module_columns(self):
        self.assertEqual(
            "/Game/All/0000000001/SkeletalMeshes/0000000001",
            self.catalog["items"]["2000"]["meshesByModule"]["1"])
        self.assertEqual(
            "/Game/All/0000610002/SkeletalMeshes/0000610002",
            self.catalog["items"]["464"]["meshesByModule"]["1"])

    def test_zero_modules_are_absent(self):
        self.assertNotIn("1",
                         self.catalog["items"]["200"]["meshesByModule"])

    def test_module_suffix_is_preserved_verbatim(self):
        self.assertEqual(
            "/Game/All/02060001_/SkeletalMeshes/02060001_",
            self.catalog["items"]["200"]["meshesByModule"]["2"])

    def test_non_player_keeps_static_mesh_fallback(self):
        warrior = self.catalog["characters"]["5"]
        self.assertEqual(4, warrior["modalType"])
        self.assertEqual(
            "/Game/All/0005000000/SkeletalMeshes/0005000000",
            warrior["staticMesh"])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests and verify RED**

Run:

```bash
python3 -m unittest CorsairsUE.Scripts.tests.test_build_character_map -v
```

Expected: import succeeds, then tests fail because `build_catalog` and the new JSON fields do not exist.

- [ ] **Step 3: Refactor the generator into pure functions**

Implement these exact helpers in `build_character_map.py`:

```python
CONTENT_ROOT = "/Game/All"
ANIMATION_ROOT = "/Game/Animations"
PLAYER_MODAL_TYPE = 1
VISIBLE_PART_COUNT = 5
MODULE_COLUMNS = ("module_1", "module_2", "module_3", "module_4")


def mesh_path(module):
    if module is None:
        return None
    value = str(module)
    if not value or value == "0":
        return None
    return f"{CONTENT_ROOT}/{value}/SkeletalMeshes/{value}"


def parse_item_ids(value):
    fields = [] if value is None else str(value).split(",")
    result = [int(field or 0) for field in fields[:VISIBLE_PART_COUNT]]
    return result + [0] * (VISIBLE_PART_COUNT - len(result))


def static_mesh_path(model, suit_id):
    asset = f"{int(model) * 1_000_000 + int(suit_id or 0) * 10_000:010d}"
    return mesh_path(asset)


def build_catalog(db_path):
    db = sqlite3.connect(db_path)
    db.row_factory = sqlite3.Row
    try:
        character_rows = db.execute(
            "SELECT id, name, modal_type, model, suit_id, skin_info "
            "FROM characters ORDER BY id").fetchall()
        item_rows = db.execute(
            "SELECT id, module_1, module_2, module_3, module_4 "
            "FROM items ORDER BY id").fetchall()
    finally:
        db.close()

    items = {}
    for row in item_rows:
        meshes = {}
        for index, column in enumerate(MODULE_COLUMNS, start=1):
            resolved = mesh_path(row[column])
            if resolved is not None:
                meshes[str(index)] = resolved
        if meshes:
            items[str(row["id"])] = {"meshesByModule": meshes}

    characters = {}
    for row in character_rows:
        model = int(row["model"])
        bone = f"{model:04d}"
        characters[str(row["id"])] = {
            "animation": (
                f"{ANIMATION_ROOT}/{bone}/SkeletalMeshes/{bone}_Anim"),
            "defaultItemIds": parse_item_ids(row["skin_info"]),
            "driverMesh": (
                f"{ANIMATION_ROOT}/{bone}/SkeletalMeshes/{bone}"),
            "modalType": int(row["modal_type"]),
            "modelId": model,
            "moduleIndex": model + 1 if 0 <= model < 4 else 0,
            "name": row["name"],
            "staticMesh": static_mesh_path(model, row["suit_id"]),
        }

    return {"characters": characters, "items": items}
```

Make `main()` call `build_catalog(db_path)` and serialize it with:

```python
json.dump(catalog, handle, ensure_ascii=False, indent=1, sort_keys=True)
```

Do not normalize module strings or call `int()` on them.

- [ ] **Step 4: Regenerate the catalog and verify GREEN**

Run:

```bash
python3 CorsairsUE/Scripts/build_character_map.py \
  databases/gamedata.sqlite CorsairsUE/Data/character_map.json
python3 -m unittest CorsairsUE.Scripts.tests.test_build_character_map -v
```

Expected: five tests pass; generated JSON is deterministic.

- [ ] **Step 5: Verify determinism**

Save the first generated file, run the generator a second time, and compare:

```bash
cp CorsairsUE/Data/character_map.json /tmp/corsairs-character-map-first.json
python3 CorsairsUE/Scripts/build_character_map.py \
  databases/gamedata.sqlite CorsairsUE/Data/character_map.json
cmp /tmp/corsairs-character-map-first.json \
    CorsairsUE/Data/character_map.json
```

Expected: `cmp` exits zero.

- [ ] **Step 6: Commit**

```bash
git add \
  CorsairsUE/Scripts/build_character_map.py \
  CorsairsUE/Scripts/tests/__init__.py \
  CorsairsUE/Scripts/tests/test_build_character_map.py \
  CorsairsUE/Data/character_map.json
git commit -m "feat(ue): generate modular character appearance catalog"
```

---

### Task 2: Preserve authoritative character look in the network session

**Files:**
- Create: `CorsairsUE/Source/CorsairsNet/Private/CorsairsLookAdapter.h`
- Create: `CorsairsUE/Source/CorsairsNet/Private/CorsairsLookAdapter.cpp`
- Create: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsLookAdapterTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp`

**Interfaces:**
- Produces: `FCorsairsCharacterLook` with exactly 34 equipment identifiers.
- Produces: `FCorsairsWorldActor UCorsairsSession::GetLocalActor() const`.
- Produces: `FCorsairsActorLookChanged OnActorLookChanged`.
- Internal adapter: `FCorsairsCharacterLook MakeCharacterLook(const Corsairs::Net::Msg::ChaLookInfo&)`.

- [ ] **Step 1: Add the RED automation test**

Create `CorsairsLookAdapterTests.cpp`:

```cpp
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CorsairsLookAdapter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCorsairsLookAdapterTest,
    "Corsairs.Character.LookAdapter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsLookAdapterTest::RunTest(const FString&)
{
    Corsairs::Net::Msg::ChaLookInfo Source;
    Source.synType = 1;
    Source.typeId = 3;
    Source.hairId = 2124;
    Source.isBoat = false;
    for (int32 Index = 0; Index < Corsairs::Net::Msg::EQUIP_NUM; ++Index)
    {
        Source.equips[Index].id = 1000 + Index;
    }

    const FCorsairsCharacterLook Result = MakeCharacterLook(Source);
    TestEqual(TEXT("syn type"), Result.SynType, 1);
    TestEqual(TEXT("type"), Result.TypeId, 3);
    TestEqual(TEXT("hair"), Result.HairId, 2124);
    TestFalse(TEXT("human"), Result.bIsBoat);
    TestEqual(TEXT("all slots preserved"), Result.EquipIds.Num(), 34);
    for (int32 Index = 0; Index < Result.EquipIds.Num(); ++Index)
    {
        TestEqual(FString::Printf(TEXT("slot %d"), Index),
                  Result.EquipIds[Index], 1000 + Index);
    }
    return true;
}

#endif
```

- [ ] **Step 2: Build and verify RED**

Run the editor build command from Task 1's worktree:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  -Project="$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
```

Expected: compilation fails because the look adapter and Unreal-facing look type do not exist.

- [ ] **Step 3: Add the Unreal-facing data model**

In `CorsairsSession.h`, add:

```cpp
inline constexpr int32 CorsairsEquipSlotCount = 34;

USTRUCT(BlueprintType)
struct FCorsairsCharacterLook
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
    int32 SynType = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
    int32 TypeId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
    int32 HairId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
    bool bIsBoat = false;

    UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
    TArray<int32> EquipIds;
};
```

Add `UPROPERTY(BlueprintReadOnly) FCorsairsCharacterLook Look;` to
`FCorsairsWorldActor`.

Add:

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FCorsairsActorLookChanged,
    int64, WorldId,
    const FCorsairsCharacterLook&, Look);
```

Expose:

```cpp
UFUNCTION(BlueprintPure, Category = "Corsairs")
FCorsairsWorldActor GetLocalActor() const { return LocalActor; }

UPROPERTY(BlueprintAssignable, Category = "Corsairs")
FCorsairsActorLookChanged OnActorLookChanged;
```

Store `FCorsairsWorldActor LocalActor;` next to `WorldId`.

- [ ] **Step 4: Implement the protocol adapter**

Implement `CorsairsLookAdapter.h/.cpp`:

```cpp
#pragma once

#include "CorsairsSession.h"
#include "CorsairsNet/include/CommandMessages.h"

FCorsairsCharacterLook MakeCharacterLook(
    const Corsairs::Net::Msg::ChaLookInfo& Source);
```

```cpp
#include "CorsairsLookAdapter.h"

FCorsairsCharacterLook MakeCharacterLook(
    const Corsairs::Net::Msg::ChaLookInfo& Source)
{
    FCorsairsCharacterLook Result;
    Result.SynType = static_cast<int32>(Source.synType);
    Result.TypeId = static_cast<int32>(Source.typeId);
    Result.HairId = static_cast<int32>(Source.hairId);
    Result.bIsBoat = Source.isBoat;
    Result.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);
    for (int32 Index = 0; Index < CorsairsEquipSlotCount; ++Index)
    {
        Result.EquipIds[Index] =
            static_cast<int32>(Source.equips[Index].id);
    }
    return Result;
}
```

- [ ] **Step 5: Populate local and remote actors**

In the successful `CMD_MC_ENTERMAP` path, populate every local actor field from
`Data.baseInfo`, including:

```cpp
LocalActor.WorldId = Data.baseInfo.worldId;
LocalActor.Name = ToFString(Data.baseInfo.name);
LocalActor.Position = FIntPoint(
    static_cast<int32>(Data.baseInfo.posX),
    static_cast<int32>(Data.baseInfo.posY));
LocalActor.Angle = static_cast<int32>(Data.baseInfo.angle);
LocalActor.TypeId = static_cast<int32>(Data.baseInfo.look.typeId);
LocalActor.CtrlType = static_cast<int32>(Data.baseInfo.ctrlType);
LocalActor.ChaId = static_cast<int32>(Data.baseInfo.chaId);
LocalActor.Handle = Data.baseInfo.handle;
LocalActor.Look = MakeCharacterLook(Data.baseInfo.look);
```

Populate `Hp` from the same attribute loop already used for visible actors.
Set `WorldId`, `SpawnPosition`, and `MapName` from `LocalActor`/`Data` without
keeping duplicate parsing logic.

In `CMD_MC_CHABEGINSEE`, replace the manual look type assignment with:

```cpp
Actor.Look = MakeCharacterLook(Message.base.look);
Actor.TypeId = Actor.Look.TypeId;
```

Keep the existing NPC fallback `Actor.TypeId = Actor.ChaId` only when the
look type is zero.

- [ ] **Step 6: Apply LOOK switch notifications**

Before the `ActionSkillTarData` branch in `CMD_MC_NOTIACTION`, add:

```cpp
if (const auto* Look =
        std::get_if<Corsairs::Net::Msg::ChaLookInfo>(&Message.data))
{
    const FCorsairsCharacterLook Converted = MakeCharacterLook(*Look);
    if (Message.worldId == WorldId)
    {
        LocalActor.Look = Converted;
        LocalActor.TypeId = Converted.TypeId;
    }
    else if (FCorsairsWorldActor* Actor = VisibleActors.FindByPredicate(
                 [&Message](const FCorsairsWorldActor& Candidate)
                 { return Candidate.WorldId == Message.worldId; }))
    {
        Actor->Look = Converted;
        Actor->TypeId = Converted.TypeId;
    }
    OnActorLookChanged.Broadcast(Message.worldId, Converted);
    return;
}
```

Reset `LocalActor` to its default value on a fresh login and logout.

- [ ] **Step 7: Build, run the automation test, and verify GREEN**

Build, then run:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Character.LookAdapter" \
  -TestExit="Automation Test Queue Empty"
```

Expected: build succeeds and the automation test passes.

- [ ] **Step 8: Commit**

```bash
git add CorsairsUE/Source/CorsairsNet
git commit -m "feat(ue): preserve authoritative character appearance"
```

---

### Task 3: Resolve character appearances through one catalog

**Files:**
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacterCatalog.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacterCatalog.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCharacterCatalogTests.cpp`

**Interfaces:**
- Consumes: `FCorsairsCharacterLook` and `Data/character_map.json`.
- Produces: `ECorsairsBodyPart`, `FCorsairsResolvedAppearance`, and `FCorsairsCharacterCatalog::Resolve`.

- [ ] **Step 1: Write the RED catalog tests**

Create an automation test that loads the real generated catalog:

```cpp
#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacterCatalog.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCorsairsCharacterCatalogTest,
    "Corsairs.Character.Catalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterCatalogTest::RunTest(const FString&)
{
    FCorsairsCharacterCatalog Catalog;
    FString Error;
    TestTrue(TEXT("catalog loads"),
             Catalog.Load(FPaths::ProjectDir() /
                          TEXT("Data/character_map.json"), Error));
    TestTrue(TEXT("load error empty"), Error.IsEmpty());

    FCorsairsCharacterLook Look;
    Look.TypeId = 1;
    Look.HairId = 2000;
    Look.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);

    FCorsairsResolvedAppearance Default;
    TestTrue(TEXT("Lambert resolves"),
             Catalog.Resolve(1, Look, Default, Error));
    TestTrue(TEXT("modular"), Default.bModular);
    TestEqual(TEXT("head"),
              Default.PartMeshes[0].ToString(),
              FString(TEXT("/Game/All/0000000001/SkeletalMeshes/0000000001")));
    TestEqual(TEXT("face"),
              Default.PartMeshes[1].ToString(),
              FString(TEXT("/Game/All/0000000000/SkeletalMeshes/0000000000")));
    TestEqual(TEXT("default body"),
              Default.PartMeshes[2].ToString(),
              FString(TEXT("/Game/All/0000610002/SkeletalMeshes/0000610002")));

    Look.EquipIds[2] = 289;
    FCorsairsResolvedAppearance Equipped;
    TestTrue(TEXT("equipped resolves"),
             Catalog.Resolve(1, Look, Equipped, Error));
    TestEqual(TEXT("equipped body"),
              Equipped.PartMeshes[2].ToString(),
              FString(TEXT("/Game/All/0000000002/SkeletalMeshes/0000000002")));

    Look.EquipIds[21] = 464;
    FCorsairsResolvedAppearance Apparel;
    TestTrue(TEXT("apparel resolves"),
             Catalog.Resolve(1, Look, Apparel, Error));
    TestEqual(TEXT("apparel overrides equipment"),
              Apparel.PartMeshes[2].ToString(),
              FString(TEXT("/Game/All/0000610002/SkeletalMeshes/0000610002")));

    Look.EquipIds[21] = 999999;
    FCorsairsResolvedAppearance Missing;
    TestTrue(TEXT("missing apparel falls back"),
             Catalog.Resolve(1, Look, Missing, Error));
    TestEqual(TEXT("fallback body"),
              Missing.PartMeshes[2].ToString(),
              FString(TEXT("/Game/All/0000610002/SkeletalMeshes/0000610002")));
    TestTrue(TEXT("fallback warning recorded"), Missing.Warnings.Num() > 0);

    FCorsairsCharacterLook NpcLook;
    FCorsairsResolvedAppearance Npc;
    TestTrue(TEXT("NPC resolves"),
             Catalog.Resolve(5, NpcLook, Npc, Error));
    TestFalse(TEXT("NPC is single mesh"), Npc.bModular);
    TestEqual(TEXT("NPC mesh"),
              Npc.StaticMesh.ToString(),
              FString(TEXT("/Game/All/0005000000/SkeletalMeshes/0005000000")));
    return true;
}

#endif
```

- [ ] **Step 2: Build and verify RED**

Run the editor build. Expected: missing catalog types cause compilation failure.

- [ ] **Step 3: Define the catalog API**

Create `CorsairsCharacterCatalog.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "CorsairsSession.h"

enum class ECorsairsBodyPart : uint8
{
    Head = 0,
    Face,
    Body,
    Gloves,
    Shoes,
    Count
};

struct FCorsairsResolvedAppearance
{
    bool bModular = false;
    FSoftObjectPath DriverMesh;
    FSoftObjectPath Animation;
    TStaticArray<FSoftObjectPath,
                 static_cast<int32>(ECorsairsBodyPart::Count)> PartMeshes;
    FSoftObjectPath StaticMesh;
    TArray<FString> Warnings;
};

class CORSAIRSGAME_API FCorsairsCharacterCatalog
{
public:
    bool Load(const FString& JsonPath, FString& OutError);
    bool Resolve(
        int32 ArchetypeId,
        const FCorsairsCharacterLook& Look,
        FCorsairsResolvedAppearance& OutAppearance,
        FString& OutError) const;

private:
    struct FCharacterEntry
    {
        int32 ModalType = 0;
        int32 ModuleIndex = 0;
        FSoftObjectPath DriverMesh;
        FSoftObjectPath Animation;
        TStaticArray<int32, 5> DefaultItemIds = {0, 0, 0, 0, 0};
        FSoftObjectPath StaticMesh;
    };

    TMap<int32, FCharacterEntry> Characters;
    TMap<int32, TMap<int32, FSoftObjectPath>> ItemMeshes;
};
```

- [ ] **Step 4: Implement one-time JSON loading**

In `Load`:

1. clear both maps;
2. read the file with `FFileHelper::LoadFileToString`;
3. parse with `FJsonSerializer`;
4. require top-level `characters` and `items` objects;
5. parse every numeric key with `LexTryParseString`;
6. require exactly five `defaultItemIds`;
7. parse item `meshesByModule` numeric keys;
8. set `OutError` to a path-specific message and return `false` on the first malformed required field;
9. return `true` only when both maps are non-empty.

Use `TryGetNumberField`, `TryGetStringField`, `TryGetArrayField`, and
`TryGetObjectField`; do not use throwing JSON getters.

- [ ] **Step 5: Implement deterministic resolution**

Use this slot algorithm:

```cpp
constexpr int32 ApparelOffset = 19;
constexpr int32 VisiblePartCount = 5;

int32 Requested[VisiblePartCount] = {};
Requested[0] =
    Look.EquipIds.IsValidIndex(0) && Look.EquipIds[0] != 0
        ? Look.EquipIds[0]
        : (Look.HairId != 0 ? Look.HairId : Entry.DefaultItemIds[0]);
for (int32 Slot = 1; Slot < VisiblePartCount; ++Slot)
{
    Requested[Slot] =
        Look.EquipIds.IsValidIndex(Slot) && Look.EquipIds[Slot] != 0
            ? Look.EquipIds[Slot]
            : Entry.DefaultItemIds[Slot];
}
for (int32 Slot = 0; Slot < VisiblePartCount; ++Slot)
{
    const int32 ApparelSlot = Slot + ApparelOffset;
    if (Look.EquipIds.IsValidIndex(ApparelSlot) &&
        Look.EquipIds[ApparelSlot] != 0)
    {
        Requested[Slot] = Look.EquipIds[ApparelSlot];
    }
}
```

For modal type `1`, resolve every requested item through `ItemMeshes[itemId][moduleIndex]`.
If a requested non-default item is missing, add a warning containing archetype,
slot, and item identifier, then resolve the default item. If the default is also
missing, leave that part path empty and add a second warning. Set driver and
animation from the character entry and set `bModular = true`.

For other modal types, set `bModular = false`, copy `StaticMesh`, `DriverMesh`,
and `Animation`, and require `StaticMesh` to be non-null.

- [ ] **Step 6: Build and verify GREEN**

Build and run:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Character.Catalog" \
  -TestExit="Automation Test Queue Empty"
```

Expected: catalog automation test passes.

- [ ] **Step 7: Commit**

```bash
git add \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacterCatalog.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacterCatalog.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCharacterCatalogTests.cpp
git commit -m "feat(ue): resolve modular character appearances"
```

---

### Task 4: Render modular actors and integrate GameMode

**Files:**
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacter.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacter.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCharacterActorTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsGameMode.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp`

**Interfaces:**
- Produces: `ACorsairsCharacter::ApplyAppearance`.
- Changes: `ACorsairsPlayerCharacter` derives from `ACorsairsCharacter`.
- Consumes: session local/remote look and `FCorsairsCharacterCatalog`.

- [ ] **Step 1: Write the RED actor automation test**

Create a test that:

1. loads `Data/character_map.json`;
2. resolves archetype 1 with body `289`, gloves `465`, shoes `641`, hair `2000`, face `255`;
3. creates a transient game world and spawns `ACorsairsCharacter`;
4. calls `ApplyAppearance`;
5. asserts five non-null skeletal meshes;
6. asserts every part's `LeaderPoseComponent` is `GetMesh()`;
7. changes only body to default `464`, reapplies, and asserts all five leader links remain.

Create and destroy the world in the test:

```cpp
UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
World->InitializeNewWorld(
    UWorld::InitializationValues()
        .AllowAudioPlayback(false)
        .RequiresHitProxies(false)
        .CreatePhysicsScene(false)
        .CreateNavigation(false)
        .CreateAISystem(false)
        .ShouldSimulatePhysics(false)
        .SetTransactional(false));
ACorsairsCharacter* Actor =
    World->SpawnActor<ACorsairsCharacter>();
TestNotNull(TEXT("actor spawned"), Actor);
```

Use these assertions, then call `World->DestroyWorld(false)` before returning:

```cpp
TestEqual(TEXT("five parts"),
          Actor->GetVisiblePartCount(), 5);
for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
{
    USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
    TestNotNull(FString::Printf(TEXT("part %d"), Slot), Part);
    TestNotNull(FString::Printf(TEXT("mesh %d"), Slot),
                Part->GetSkeletalMeshAsset());
    TestEqual(FString::Printf(TEXT("leader %d"), Slot),
              Part->LeaderPoseComponent.Get(), Actor->GetMesh());
}
```

- [ ] **Step 2: Build and verify RED**

Run the editor build. Expected: missing `ACorsairsCharacter` API fails compilation.

- [ ] **Step 3: Create the shared character base**

Define:

```cpp
UCLASS()
class CORSAIRSGAME_API ACorsairsCharacter : public ACharacter
{
    GENERATED_BODY()

public:
    ACorsairsCharacter();
    bool ApplyAppearance(const FCorsairsResolvedAppearance& Appearance);
    USkeletalMeshComponent* GetPartComponent(int32 Slot) const;
    int32 GetVisiblePartCount() const { return VisibleParts.Num(); }

protected:
    bool PlayAppearanceAnimation(const FSoftObjectPath& AnimationPath);
    void HideVisibleParts();
    void ApplySharedScale();

private:
    UPROPERTY(VisibleAnywhere, Category = "Corsairs")
    TArray<TObjectPtr<USkeletalMeshComponent>> VisibleParts;
};
```

The constructor must create five named default subobjects (`Head`, `Face`,
`Body`, `Gloves`, `Shoes`), attach them to `GetMesh()`, disable their collision,
and set:

```cpp
GetMesh()->VisibilityBasedAnimTickOption =
    EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
GetMesh()->SetRelativeLocation(FVector(0.0, 0.0, -88.0));
GetMesh()->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));
```

- [ ] **Step 4: Implement modular and single-mesh appearance**

For modular appearance:

1. load `DriverMesh` into `GetMesh()`;
2. set the driver invisible with `SetVisibility(false, false)` but keep
   `AlwaysTickPoseAndRefreshBones`;
3. load each non-empty part into its matching component;
4. clear an empty part with `SetSkeletalMesh(nullptr)`;
5. after every set, call:

```cpp
Part->SetLeaderPoseComponent(GetMesh(), true, false);
Part->SetVisibility(true, false);
```

For a single-mesh appearance:

1. clear/hide all five parts;
2. load `StaticMesh` into `GetMesh()`;
3. make `GetMesh()` visible.

For both paths, load the `UAnimSequence` and play it only on `GetMesh()` with
`AnimationSingleNode` and looping enabled. Because driver and animation come
from the same `/Game/Animations/<model>` import, remove the editor-only
`USkeleton::IsCompatibleForEditor` call.

Compute one uniform scale from the union of the five asset-local bounds for a
modular actor, or the single mesh bounds for an NPC. Set only the driver scale:

```cpp
const double Factor = 176.0 / FMath::Max(CombinedHeight, 1.0);
GetMesh()->SetRelativeScale3D(FVector(Factor));
```

Log and return `false` when the driver, animation, or required single mesh fails
to load. Missing modular parts are already represented by catalog warnings and
do not fail the complete actor.

- [ ] **Step 5: Move player behavior onto the shared base**

Change:

```cpp
class CORSAIRSGAME_API ACorsairsPlayerCharacter : public ACorsairsCharacter
```

Remove `SetBodyMesh` and `SetBodyAnimation`; their responsibilities now live in
`ApplyAppearance`. Keep player movement, terrain-height binding, camera, login
input, and server reporting unchanged.

- [ ] **Step 6: Integrate one cached catalog in GameMode**

In `ACorsairsGameMode`:

- add `TUniquePtr<FCorsairsCharacterCatalog> CharacterCatalog`;
- load it once from `FPaths::ProjectDir() / "Data/character_map.json"` in
  `BeginPlay`, logging the full error and stopping auto-login if load fails;
- subscribe to `Session->OnActorLookChanged`;
- use `Session->GetLocalActor()` in `InWorld`, not the first valid character
  slot;
- resolve and apply the local appearance before attaching the session;
- resolve every `FCorsairsWorldActor` in `HandleActorSeen`;
- spawn `ACorsairsCharacter` for remote actors, not
  `ACorsairsPlayerCharacter`;
- on look change, resolve again and call `ApplyAppearance` on the existing
  world actor;
- remove `ResolveBodyMesh`, `ResolveField`, and per-call JSON parsing;
- wrap `SetActorLabel` in `#if WITH_EDITOR` / `#endif`.

For catalog warnings, log every string once at application time with actor name
and world identifier.

- [ ] **Step 7: Build and verify GREEN**

Build, then run:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Character" \
  -TestExit="Automation Test Queue Empty"
```

Expected: look, catalog, and actor tests pass.

- [ ] **Step 8: Commit**

```bash
git add CorsairsUE/Source/CorsairsGame
git commit -m "feat(ue): render modular character appearances"
```

---

### Task 5: Apply the declared original camera profile

**Files:**
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCameraProfile.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCameraProfile.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCameraProfileTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp`

**Interfaces:**
- Produces: `Corsairs::Game::Camera::DeriveRig`.
- Applies: 6103.2778 cm arm, -55.00798° pitch, +90° yaw, 54.0222° horizontal FOV at 16:9, +12 cm boom offset from capsule center.

- [ ] **Step 1: Write the RED pure camera test**

Create:

```cpp
#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCameraProfile.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCorsairsCameraProfileTest,
    "Corsairs.Camera.Profile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCameraProfileTest::RunTest(const FString&)
{
    using namespace Corsairs::Game::Camera;
    const FCameraProfile Profile = LegacyDefaultProfile();
    const FCameraRig Rig = DeriveRig(Profile, 16.0 / 9.0);
    TestEqual(TEXT("horizontal"), Profile.HorizontalOffsetCm, 3500.0);
    TestEqual(TEXT("vertical"), Profile.VerticalOffsetCm, 5000.0);
    TestEqual(TEXT("vertical FOV"), Profile.VerticalFovDegrees, 32.0);
    TestEqual(TEXT("target height"), Profile.TargetHeightCm, 100.0);
    TestEqual(TEXT("initial yaw"), Profile.InitialYawDegrees, 90.0);
    TestTrue(TEXT("arm"),
             FMath::IsNearlyEqual(Rig.ArmLengthCm, 6103.2778, 0.001));
    TestTrue(TEXT("pitch"),
             FMath::IsNearlyEqual(Rig.PitchDegrees, -55.00798, 0.001));
    TestTrue(TEXT("horizontal FOV"),
             FMath::IsNearlyEqual(
                 Rig.HorizontalFovDegrees, 54.0222067, 0.001));
    return true;
}

#endif
```

- [ ] **Step 2: Build and verify RED**

Run the editor build. Expected: missing profile API fails compilation.

- [ ] **Step 3: Implement pure camera derivation**

Create:

```cpp
namespace Corsairs::Game::Camera
{
struct FCameraProfile
{
    double HorizontalOffsetCm;
    double VerticalOffsetCm;
    double VerticalFovDegrees;
    double TargetHeightCm;
    double InitialYawDegrees;
};

struct FCameraRig
{
    double ArmLengthCm;
    double PitchDegrees;
    double HorizontalFovDegrees;
};

CORSAIRSGAME_API FCameraProfile LegacyDefaultProfile();
CORSAIRSGAME_API FCameraRig DeriveRig(
    const FCameraProfile& Profile,
    double ReferenceAspectRatio);
}
```

Implementation:

```cpp
FCameraProfile LegacyDefaultProfile()
{
    return {3500.0, 5000.0, 32.0, 100.0, 90.0};
}

FCameraRig DeriveRig(
    const FCameraProfile& Profile,
    double ReferenceAspectRatio)
{
    const double VerticalRadians =
        FMath::DegreesToRadians(Profile.VerticalFovDegrees);
    return {
        FMath::Sqrt(FMath::Square(Profile.HorizontalOffsetCm) +
                    FMath::Square(Profile.VerticalOffsetCm)),
        -FMath::RadiansToDegrees(FMath::Atan2(
            Profile.VerticalOffsetCm,
            Profile.HorizontalOffsetCm)),
        FMath::RadiansToDegrees(
            2.0 * FMath::Atan(
                FMath::Tan(VerticalRadians * 0.5) *
                ReferenceAspectRatio))
    };
}
```

- [ ] **Step 4: Apply the rig to the player camera**

In the player constructor:

```cpp
const auto Profile = Corsairs::Game::Camera::LegacyDefaultProfile();
const auto Rig = Corsairs::Game::Camera::DeriveRig(Profile, 16.0 / 9.0);
CameraBoom->TargetArmLength = Rig.ArmLengthCm;
CameraBoom->bDoCollisionTest = false;
CameraBoom->SetRelativeLocation(FVector(
    0.0, 0.0, Profile.TargetHeightCm - 88.0));
FollowCamera->FieldOfView = Rig.HorizontalFovDegrees;
FollowCamera->AspectRatio = 16.0f / 9.0f;
FollowCamera->bOverrideAspectRatioAxisConstraint = true;
FollowCamera->AspectRatioAxisConstraint =
    EAspectRatioAxisConstraint::AspectRatio_MaintainYFOV;
```

In `BeginPlay`, set controller rotation to:

```cpp
OwningController->SetControlRotation(FRotator(
    Rig.PitchDegrees,
    Profile.InitialYawDegrees,
    0.0));
```

Set `ViewPitchMin` and `ViewPitchMax` to `Rig.PitchDegrees` because the original
default camera has no free pitch. Remove the duplicate `CameraPitch` constant
and control-rotation write from `GameMode`.

- [ ] **Step 5: Build and verify GREEN**

Build and run `Automation RunTests Corsairs.Camera.Profile`. Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsCameraProfile.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsCameraProfile.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCameraProfileTests.cpp \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp
git commit -m "fix(ue): match the original default camera profile"
```

---

### Task 6: Eliminate Garner material usage fallbacks

**Files:**
- Create: `CorsairsUE/Scripts/material_usage_rules.py`
- Create: `CorsairsUE/Scripts/material_usage_editor.py`
- Create: `CorsairsUE/Scripts/tests/test_material_usage_rules.py`
- Create: `CorsairsUE/Scripts/check_material_usage.py`
- Create: `CorsairsUE/Scripts/fix_material_usage.py`
- Modify: `CorsairsUE/Scripts/check_object_materials.py`

**Interfaces:**
- Produces pure `required_usages(component_kind, nanite_enabled, blend_mode)`.
- Produces read-only `check_material_usage.py [map]`.
- Produces idempotent `fix_material_usage.py [map]`.

- [ ] **Step 1: Write the RED pure rule tests**

Create:

```python
import unittest

from CorsairsUE.Scripts.material_usage_rules import required_usages


class MaterialUsageRulesTests(unittest.TestCase):
    def test_opaque_nanite_hism_needs_both_usages(self):
        self.assertEqual(
            {"instanced_static_meshes", "nanite"},
            required_usages("hism", True, "opaque"))

    def test_translucent_hism_disallows_nanite(self):
        self.assertEqual(
            {"instanced_static_meshes", "disallow_nanite"},
            required_usages("hism", True, "translucent"))

    def test_opaque_non_nanite_hism_needs_instancing_only(self):
        self.assertEqual(
            {"instanced_static_meshes"},
            required_usages("hism", False, "opaque"))

    def test_nanite_terrain_needs_nanite_usage(self):
        self.assertEqual(
            {"nanite"},
            required_usages("terrain", True, "opaque"))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run and verify RED**

Run:

```bash
python3 -m unittest CorsairsUE.Scripts.tests.test_material_usage_rules -v
```

Expected: import fails because `material_usage_rules.py` does not exist.

- [ ] **Step 3: Implement pure usage rules**

Create:

```python
def required_usages(component_kind, nanite_enabled, blend_mode):
    required = set()
    if component_kind == "hism":
        required.add("instanced_static_meshes")
    if not nanite_enabled:
        return required
    if blend_mode == "translucent":
        required.add("disallow_nanite")
    else:
        required.add("nanite")
    return required
```

Run the unit test again. Expected: four tests pass.

- [ ] **Step 4: Implement the read-only editor checker**

Put Unreal-only level/component/material traversal in
`material_usage_editor.py`; this module may import `unreal` but must not execute
a check or save at import time. `check_material_usage.py` calls those helpers
from its guarded `main(report)` entry point and must:

1. load `/Game/Maps/<map>`;
2. iterate all level actors and their
   `HierarchicalInstancedStaticMeshComponent` and tagged terrain static-mesh
   components with non-zero instances/valid meshes;
3. inspect effective component materials;
4. get the base material blend mode with
   `material.get_base_material().get_editor_property("blend_mode")`;
5. detect active Nanite from mesh `nanite_settings.enabled` and component
   `disallow_nanite`;
6. map required flags through `required_usages`;
7. verify effective flags with:

```python
unreal.MaterialEditingLibrary.has_material_usage(
    material,
    unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES)
unreal.MaterialEditingLibrary.has_material_usage(
    material,
    unreal.MaterialUsage.MATUSAGE_NANITE)
```

8. report unique material paths, missing instancing, missing Nanite,
   translucent Nanite components, and missing `BaseColorTexture` separately;
9. emit `report.error` when any of the first three failure categories is non-zero;
10. never save an asset or level.

- [ ] **Step 5: Run checker and capture the RED baseline**

Run:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/check_material_usage.py Garner" \
  -unattended -nop4 -NullRHI -NoSound
```

Expected: non-zero process status and a report containing missing Instanced
Static Mesh and Nanite usage.

- [ ] **Step 6: Implement the idempotent fixer**

`fix_material_usage.py` must:

1. call `RefuseIfEditorOpen`;
2. load the requested map and enumerate components through
   `material_usage_editor`;
3. for each opaque/masked required MIC, call:

```python
unreal.MaterialEditingLibrary.set_material_usage_override(
    material,
    unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,
    True, True)
unreal.MaterialEditingLibrary.set_material_usage_override(
    material,
    unreal.MaterialUsage.MATUSAGE_NANITE,
    True, True)
```

4. set only the flags returned by `required_usages`;
5. for translucent Nanite components, set:

```python
component.set_editor_property("disallow_nanite", True)
```

6. verify every changed flag by re-reading
   `has_material_usage`/`has_material_usage_override`;
7. save each changed MIC once and save the level once when component properties
   changed;
8. report `ИСПРАВЛЕНО_МАТЕРИАЛОВ` and `ИСПРАВЛЕНО_КОМПОНЕНТОВ`;
9. on a second run report both counts as zero.

Do not reparent a MIC, change its texture parameter, or alter blend mode.

- [ ] **Step 7: Apply, verify GREEN, and verify idempotency**

Run the fixer, checker, fixer again:

```bash
UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
PROJECT="$PWD/CorsairsUE/CorsairsUE.uproject"

"$UE" "$PROJECT" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/fix_material_usage.py Garner" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PROJECT" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/check_material_usage.py Garner" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PROJECT" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/fix_material_usage.py Garner" \
  -unattended -nop4 -NullRHI -NoSound
```

Expected: checker has zero usage failures; second fixer reports zero changes.

- [ ] **Step 8: Strengthen the existing object checker**

Change `check_object_materials.py` so any HISM material with a placeholder or
missing effective `BaseColorTexture` is listed, and any usage-fallback failure
is an error independent of the old “more than half” threshold. Keep missing
texture counts separate from usage counts.

- [ ] **Step 9: Commit reproducible source changes**

`Content` and the modified level remain ignored local artifacts. Commit only
the scripts and tests:

```bash
git add \
  CorsairsUE/Scripts/material_usage_rules.py \
  CorsairsUE/Scripts/material_usage_editor.py \
  CorsairsUE/Scripts/tests/test_material_usage_rules.py \
  CorsairsUE/Scripts/check_material_usage.py \
  CorsairsUE/Scripts/fix_material_usage.py \
  CorsairsUE/Scripts/check_object_materials.py
git commit -m "fix(ue): make imported material usage explicit"
```

---

## Final Verification

After all task reviews are clean:

1. Run both Python suites.
2. Build `CorsairsUEEditor Mac Development`.
3. Run automation filters `Corsairs.Character` and `Corsairs.Camera`.
4. Run the Garner material checker and verify zero usage failures.
5. Start the existing local server stack only if its ports are not already
   listening.
6. Launch the isolated Unreal project on `/Game/Maps/Garner` at
   `1400x760`, log in as the existing test account, and wait for `InWorld`.
7. Capture the current frame with Computer Use.
8. Verify the player is complete and small, the camera is distant/top-down,
   and scene objects are textured.
9. Assert that the fresh runtime log has no fallback warning:

```bash
if rg -q "Default Material will be used in game" \
  "$HOME/Library/Logs/CorsairsUE/CorsairsUE.log"; then
  echo "material fallback remains" >&2
  exit 1
fi
```

Expected: the assertion exits zero.

10. Compare the new frame to the saved original-client reference at the same
    Garner coordinates.
