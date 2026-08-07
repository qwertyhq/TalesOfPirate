# Garner Scene Placement Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild scene objects around the Garner reference position so effects never become buildings, each model part has the original anchor, height, mirrored basis, yaw, and legacy lighting, and the placement is reproducible from tracked source data.

**Architecture:** Extend the converter with stable source keys and one type policy, sample terrain through a streaming tile interface, export scene assets through a dedicated mirrored `SceneMap` glTF profile, and produce a source manifest plus catalog-driven placement manifest. Unreal imports only `/Game/SceneParity`, groups static parts deterministically as HISM, places valid skeletal parts as deterministic tagged skeletal actors, passes the same legacy-lighting payload through the material path, and saves Garner only after strict source-key gates pass.

**Tech Stack:** C++23 AssetConverter/CTest, SQLite game-data catalog, Python 3 `unittest`, glTF 2.0, Unreal Engine 5.8.1 C++/Python, HISM, UE Automation Tests.

## Global Constraints

- Work only in `/Users/ivan/code/TalesOfPirate/codex-ue-parity` on branch `qwertyhq/codex-ue-parity`.
- The approved design is `docs/superpowers/specs/2026-08-07-garner-reference-zone-parity-design.md`.
- Follow strict TDD: focused RED first, minimal GREEN, focused and full verification, independent review, then a scoped commit.
- Heavy toolchains run sequentially at the pipeline level: never overlap CMake/MSBuild/Unreal builds, automation, editor commands, or either client capture. Native compilation is capped at two workers (`-j2`, `/m:2` plus `/p:CL_MPCount=2`, `-MaxParallelActions=2`), and CTest uses `-j1`; a faster local machine is not permission to raise these limits in the parity workflow.
- `Game.exe`, CrossOver/Wine helpers owned by the run, `UnrealEditor`, and `UnrealEditor-Cmd` may exist only during their active test/capture step. Each launcher waits for process-tree exit and its `finally` path requests graceful shutdown, applies a bounded forced termination only to child PIDs it started, and verifies they are gone before the next step. A pre-existing matching process is a hard preflight failure rather than a process to kill; no idle editor/client is left after success, failure, timeout, or interruption.
- This plan depends on `MapSectionReader` from Garner terrain Task 1. Never reintroduce full-map `MapTerrain::Tiles` or `ReadWholeFile` into the production scene path.
- Object type 0 is a scene model; type 1 is a deferred effect and can never enter model lookup or HISM placement; every other type is a fatal input error.
- Stable identity is `(sectionIndex, slotIndex, byteOffset)`. Proximity matching is forbidden.
- Reference set is every type-0 anchor within 8000 cm inclusive of `(223325,278475)`. Garner baseline is exactly 1634 anchors: 1625 on island 1, 6 on island 2, and 3 in absent/default island 0 sections.
- Source `sScale` is diagnostic only. Instance scale remains positive `(1,1,1)`.
- Scene Z is `TerrainSurfaceHeight + heightOff`. This is not the character half-meter block sampler.
- Scene assets use a dedicated mirrored `/Game/SceneParity` namespace. Do not mutate generic `/Game/All` assets shared with characters/equipment. Static assets may use HISM; valid skinned assets must use skeletal actors/components because HISM cannot contain a `USkeletalMesh`.
- Before rebuilding the staging map, classify the complete legacy generated population left by `place_objects.py`/`place_all_maps.py`: untagged `Inst_*` HISM groups and exact legacy-transform `/Game/All` static/skeletal fallbacks. Delete only a complete, unambiguous generated match; preserve gameplay/unrelated actors; any partial or multiply attributable match is fatal before mutation. The final checker scans the whole world, not only tagged actors, and rejects every legacy-generated or ambiguous leftover.
- Scene yaw is `UnwindDegrees(180 - sourceYawDegrees)`. Never divide yaw by 10.
- Mirror positions, normals, winding, every resolved `.lmo` `MatModel`, dummy transforms, bind poses, inverse bind matrices, and animation tracks exactly once. SceneMap must support every skeletal asset selected by the Task 4 schema-v2 source manifest; silent fallback to a generic profile is forbidden. The mandatory real reference-model input is the ordered distinct set from `records[type == 0 && inReferenceSet == true]`, resolved through the tracked `scene_objects` table. A literal model-ID list or an all-directory crawl is not proof of reference-set coverage.
- Every skinned `.lmo` part has an explicit `part_root` carrying `G * MatModel * G`; its mesh node is identity and geometry, joints, inverse binds, and animation tracks never receive `MatModel` a second time. The real corpus baseline is 639 `.lmo`, 2021 parts, and 79 skinned parts; all 79 currently have identity resolved `MatModel`, so the required real non-identity-skinned census is exactly `[]` and may remain empty. Every real skinned part is nevertheless checked exactly once. A separate tracked synthetic skinned fixture with non-identity `MatModel` must fail on identity loss or double application and is never included in real `scene-assets.json`, the reference-model set, or any real-data census.
- Every source material has one of five explicit supported legacy modes (`opaque`, `masked`, `alpha`, `additive`, or `subtractive`) from raw `opacity`, raw `transp_type`, its original-client effective transparency type, and material render-state atoms. Preserve both `rawTranspType` and `effectiveTranspType`: the original loader normalizes raw `2` to `MTLTEX_TRANSP_SUBTRACTIVE` (`5`) before rendering. Subtractive is the distinct `ZERO/INVSRCCOLOR` equation `dst * (1 - src)`, not alpha or additive. This metadata remains authoritative through glTF extras, the scene catalog, UE import, runtime export, and capture checking; unsupported or contradictory state is fatal, never guessed from texture alpha.
- Static and skeletal SceneParity materials use separate parent families. HISM reads the common 33-float lighting payload from `PerInstanceCustomData`; every skeletal component stores the identical 33 floats in `CustomPrimitiveData`. Parent usage flags and runtime payload transport are checked independently.
- Per-model lighting flags override island-area defaults. Bell Tower ID 22 is legacy-unlit; Bench ID 314 and Garden ID 323 use area lighting.
- Three absent/island-0 objects may use inherited legacy lighting only when their projected bounds are outside the fixed reference viewport; otherwise capture is fatal.
- Controlled generation of ignored `Content/SceneParity`, `Content/Maps/Garner`, and `artifacts/scene-parity` is part of verification. A clean checkout has no usable `/Game/Maps/Garner`: Task 8 must first run the tracked Garner terrain plan Tasks 7-8 bootstrap, configure `/Script/CorsairsGame.CorsairsGameMode`, and hard-gate the resulting manifest/reports and actual terrain actor before it may duplicate a staging world. No ignored local `Content` is an implicit prerequisite. Never hand-edit or stage generated assets. Do not modify or stage `__pycache__` or `databases/game.db`. `Client/user/system.ini` may be changed only inside Task 8's locked capture transaction and must be restored to its exact original bytes/existence/mode before success or failure returns; it is never staged.
- Original capture is available only through opt-in client instrumentation. It advances a logical 30 Hz simulation timestamp, counts completed simulation/render ticks, captures inside tick 120, and atomically writes a hashed runtime-state result; a launcher sleep or external PrintScreen timing is never evidence of a tick. Capture hard-fails without the reproducible instrumented x64 build and restores `Client/user/system.ini` byte-for-byte on every exit path.

---

### Task 1: Give every source record a stable key and one type policy

**Task brief:** Parse source records once, preserve the independently parsed source header, assign immutable byte-derived identity, and publish an all-or-nothing type partition. Input is `garner.obj`; output is its exact `SceneFileHeader`, ordered type-0/type-1 records, and the 1634-key reference set; unknown types, unstable keys, or partial header publication are hard failures.

**Files:**

- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/SceneObjParser.h`
- Modify: `tools/AssetConverter/src/SceneObjParser.cpp`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/SceneParity.h`
- Create: `tools/AssetConverter/src/SceneParity.cpp`
- Create: `tools/AssetConverter/tests/TestSceneParity.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct SceneSourceKey {
    std::uint32_t SectionIndex{0};
    std::uint32_t SlotIndex{0};
    std::uint64_t ByteOffset{0};
    auto operator<=>(const SceneSourceKey&) const = default;
};

struct PlacedObject {
    SceneObjInfo Info{};
    std::uint32_t SectionX{0};
    std::uint32_t SectionY{0};
    std::uint32_t SectionWidth{0};
    std::uint32_t SectionHeight{0};
    SceneSourceKey Source{};
    std::int32_t WorldX() const;
    std::int32_t WorldY() const;
};

struct SceneSelection {
    SceneFileHeader SourceHeader{};
    std::vector<PlacedObject> Models;
    std::vector<PlacedObject> DeferredEffects;
    std::vector<SceneSourceKey> ReferenceKeys;
};

enum class SceneRecordDisposition {
    SceneModel,
    DeferredEffect,
};

enum class SceneSelectionStatus {
    OK,
    UNKNOWN_OBJECT_TYPE,
};

struct ReferenceZone {
    std::int32_t CenterX{223325};
    std::int32_t CenterY{278475};
    std::int32_t RadiusCm{8000};
};

SceneSelectionStatus BuildSceneSelection(
    const SceneObjects& scene,
    const ReferenceZone& zone,
    SceneSelection& output,
    std::string& detail);
```

- [ ] **Step 1: Add RED parser/type tests**

Create a synthetic `.obj` with section 1 `ObjInfoPos=100`, non-default `SectionCntX`, `SectionCntY`, `SectionWidth`, `SectionHeight`, `Version`, `FileSize`, and `SectionObjNum`, and two records sharing `modelId=1`, first type 0 and second type 1. Require keys `(1,0,100)` and `(1,1,120)`, one model, one deferred effect, exact per-record section dimensions, and field-for-field preservation of the independently parsed `SceneFileHeader` in `SceneSelection::SourceHeader`. Model resolution is not part of this task; the catalog test in Task 5 proves that a type-1 record never reaches lookup.

Require type 2 to return `UNKNOWN_OBJECT_TYPE` and publish no output, including no partially copied `SourceHeader`. Anchors at radii 7999, 8000, and 8001 cm have membership true, true, false. Mutating only one header field in the later Task 4 context must be detectable by comparison with this preserved header.

- [ ] **Step 2: Verify RED**

```bash
cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tools/AssetConverter/build \
  --target AssetConverterTests -j2
ctest --test-dir tools/AssetConverter/build -j1 --output-on-failure
```

Expected compile failure for missing `SceneParity.h` and `PlacedObject::Source`.

- [ ] **Step 3: Implement source keys and selection**

During parse:

```cpp
placed.Source.SectionIndex = sectionIndex;
placed.Source.SlotIndex = slotIndex;
placed.Source.ByteOffset =
    section.ObjInfoPos + slotIndex * sizeof(SceneObjInfo);
```

Build a temporary selection in source order, copy `scene.Header` into its `SourceHeader`, and publish the entire temporary only after every record has a known type and stable key. Record source scale only as a diagnostic field; never use it in a transform. Task 4 receives its context through a separate CLI data path and compares that context against this parser-owned header; it never reconstructs section metadata from the selected records.

- [ ] **Step 4: Verify GREEN and commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/SceneObjParser.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/SceneParity.h \
  tools/AssetConverter/src/SceneObjParser.cpp \
  tools/AssetConverter/src/SceneParity.cpp \
  tools/AssetConverter/tests/TestSceneParity.cpp
git commit -m "feat(converter): classify scene source records"
```

---

### Task 2: Sample the original triangular terrain surface

**Task brief:** Provide the scene-only height/island/color sampler over the paged terrain reader. Input is a source anchor and streaming `.map`; output is one auditable terrain sample without full-map allocation. Boundary and absent tiles use explicit zero-valued defaults, while I/O/format failure remains a sticky fatal adapter error visible through the generic interface to the later manifest publisher.

**Dependencies:** Garner terrain Task 1.

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainSurface.h`
- Create: `tools/AssetConverter/src/TerrainSurface.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainSurface.cpp`
- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapSectionReader.h`
- Modify: `tools/AssetConverter/src/MapSectionReader.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct TerrainTileRead {
    MapTile Tile{};
    bool SectionPresent{false};
};

class IMapTileSource {
public:
    virtual ~IMapTileSource() = default;
    virtual std::size_t GridWidth() const = 0;
    virtual std::size_t GridHeight() const = 0;
    virtual TerrainTileRead ReadTile(
        std::int32_t tileX,
        std::int32_t tileY) = 0;
    virtual const std::string& LastError() const = 0;
};

class MapSectionTileSource final : public IMapTileSource {
public:
    explicit MapSectionTileSource(MapSectionReader& reader);
    std::size_t GridWidth() const override;
    std::size_t GridHeight() const override;
    TerrainTileRead ReadTile(
        std::int32_t tileX,
        std::int32_t tileY) override;
    const std::string& LastError() const override;

private:
    MapSectionReader& Reader;
    std::optional<MapSection> CachedSection;
    std::int32_t CachedSectionX{-1};
    std::int32_t CachedSectionY{-1};
    std::string Error;
};

struct TerrainAnchorSample {
    float SurfaceHeightCm{0};
    std::uint16_t TileColor565{0};
    std::uint8_t Island{0};
    bool SectionPresent{false};
};

float TerrainSurfaceHeight(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm);

TerrainAnchorSample SampleTerrainAnchor(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm);
```

- [ ] **Step 1: Add RED sampler tests**

For corner heights `0/100/200/0 cm`, require 75 cm at both `(25,25)` and `(75,75)`, and explicitly reject 56.25 cm bilinear output. Four raw heights −10 return sea-clamped zero. Negative coordinates and right/bottom positions lacking four source vertices return zero.

Require floor tile `Color=0x7BEF`, `Island=2`, present true to preserve those values. An absent tile returns presence false, island 0, and default color. Surface 60 plus `heightOff=0` must later produce Z 60. Every in-memory fake implements `LastError()` and returns one stable empty string unless the fixture explicitly injects a sticky read failure; this makes the generic error channel testable without downcasting to `MapSectionTileSource`.

- [ ] **Step 2: Verify RED**

Expected compile failure because `TerrainSurface.h` is absent.

- [ ] **Step 3: Implement source triangles**

Use source quad triangles:

```text
(0,0),(1,0),(0,1)
(0,1),(1,0),(1,1)
```

Clamp only the final height below sea level to zero. Do not clamp coordinates to map bounds. `MapSectionTileSource::ReadTile` validates bounds, computes the owning section, loads it through the mutable streaming `MapSectionReader::ReadSection`, and keeps at most one cached section. Missing sections return `SectionPresent=false`; I/O or format failure records the first deterministic diagnostic in sticky `LastError()` and makes manifest publication fail. Successful or absent reads never clear that error. Consumers inspect it through `IMapTileSource`, not a concrete downcast. The adapter cannot create a full-map tile array.

- [ ] **Step 4: Verify GREEN and commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapSectionReader.h \
  tools/AssetConverter/src/MapSectionReader.cpp \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainSurface.h \
  tools/AssetConverter/src/TerrainSurface.cpp \
  tools/AssetConverter/tests/TestTerrainSurface.cpp
git commit -m "feat(converter): sample legacy terrain surface"
```

---

### Task 3: Export scene meshes through the mirrored `SceneMap` profile

**Task brief:** Make SceneMap a complete, self-describing asset conversion rather than a vertex-only mirror. Input is each resolved `.lgo`/`.lmo` part and optional skin; output is a validated glTF pair with one explicit mirrored part root and authoritative legacy material metadata; unsupported skin/material state aborts the run before publication.

**Dependencies:** Task 4 schema-v2 source manifest for the real reference-model gate. Parser/writer TDD may start earlier, but Task 3 cannot complete until its real required-model set has been derived from Task 4 rather than a literal list or directory contents.

**Files:**

- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/LgoParser.h`
- Modify: `tools/AssetConverter/src/LgoParser.cpp`
- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/GltfWriter.h`
- Modify: `tools/AssetConverter/src/GltfWriter.cpp`
- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h`
- Modify: `tools/AssetConverter/src/GltfSkeletonWriter.cpp`
- Modify: `tools/AssetConverter/src/Main.cpp`
- Modify: `tools/AssetConverter/tests/TestGltfWriter.cpp`
- Modify: `tools/AssetConverter/tests/TestGltfSkeletonWriter.cpp`
- Modify: `tools/AssetConverter/tests/TestLgoParser.cpp`
- Modify: `tools/AssetConverter/tests/TestLmoParser.cpp`
- Create: `tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json`
- Create: `CorsairsUE/Scripts/build_scene_reference_model_set.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_reference_model_set.py`

**Interfaces:**

```cpp
enum class GltfCoordinateProfile : std::uint8_t {
    Generic,
    SceneMap,
};

enum class GltfStatus : std::uint32_t {
    OK,
    EMPTY_MESH,
    WRITE_FAILED,
    INVALID_SKIN_DATA,
    UNSUPPORTED_MATERIAL_MODE,
};

enum class LegacyMaterialMode : std::uint8_t {
    Opaque,
    Masked,
    Alpha,
    Additive,
    Subtractive,
};

struct LgoMaterial {
    float Opacity{1.0f};
    std::uint32_t RawTranspType{0};
    std::uint32_t EffectiveTranspType{0};
    Material Mtl{};
    std::string Textures[kMaxTextureStageNum];
    std::array<RenderStateAtom, kMtlRsNum> RenderStates{};
    [[nodiscard]] std::string TextureName(std::size_t stage) const;
};

struct LegacyMaterialMetadata {
    LegacyMaterialMode Mode{LegacyMaterialMode::Opaque};
    float Opacity{1.0f};
    std::uint32_t RawTranspType{0};
    std::uint32_t EffectiveTranspType{0};
    bool AlphaTestEnabled{false};
    std::uint32_t AlphaRef{0};
    std::uint32_t AlphaFunc{0};
    bool AlphaBlendEnabled{false};
    std::uint32_t SrcBlend{0};
    std::uint32_t DestBlend{0};
};

enum class LegacyMaterialStatus : std::uint8_t {
    OK,
    UNSUPPORTED_TRANSPARENCY_TYPE,
    UNSUPPORTED_BLEND_PAIR,
    UNSUPPORTED_ALPHA_TEST,
    CONTRADICTORY_RENDER_STATE,
};

LegacyMaterialStatus ResolveLegacyMaterial(
    const LgoMaterial& material,
    LegacyMaterialMetadata& output,
    std::string& detail);

LegacyMaterialStatus NormalizeLegacyTransparencyType(
    std::uint32_t rawTranspType,
    std::uint32_t& effectiveTranspType,
    std::string& detail);

void ConvertMatrixToGltf(
    const float* input,
    float* output,
    GltfCoordinateProfile profile =
        GltfCoordinateProfile::Generic);

GltfStatus WriteGltf(
    const LgoGeomObj& object,
    const std::filesystem::path& path,
    std::string& detail,
    const GltfTextureOptions& textures = {},
    const LabAnimation* skeleton = nullptr,
    GltfCoordinateProfile profile =
        GltfCoordinateProfile::Generic);
```

The tracked real-input resolver is:

```python
def build_reference_model_set(
    source_manifest_path: str,
    db_path: str,
    model_root: str,
) -> dict: ...
```

It accepts only source-manifest schema v2, selects the ordered distinct
`modelId` values from records whose literal `type == 0` and
`inReferenceSet is True`, resolves each ID through tracked
`scene_objects.data_name`, and atomically publishes paths and SHA-256 values
for the required `.lmo` inputs. The output also records the source-manifest
path/SHA and the 1634 contributing source keys. Missing/duplicate catalog
rows, a type-1 contribution, absent model input, path escape, SHA drift, or
reference count other than 1634 is fatal and publishes nothing.

For task-by-task execution, `source_manifest_path` is the exact durable
`artifacts/scene-parity/runs/<task4Run>/garner.objects.json` printed and
recorded by Task 4 GREEN. Task 3 reads it in place and writes its downstream
outputs into the same run directory. It may not regenerate the source
manifest, substitute `artifacts/maps/garner.objects.json`, scan for the newest
run, or copy the manifest to a new path. The full orchestrator uses the same
rule with its already allocated `<run>` directory.

- [ ] **Step 1: Add RED asymmetric, skinned-part-root, material, and compatibility tests**

Use source:

```text
positions: (0,0,0), (2,0,0), (0,1,0)
normal: (0,0,1)
indices: 0,1,2
MatModel translation: (3,4,0)
```

Require literal glTF:

```text
positions: (0,0,0), (2,0,0), (0,0,-1)
normal: (0,1,0)
indices: 0,1,2
node translation: (3,0,-4)
```

Require dummy translation `(7,8,9)` to use the same profile; every part of a multi-part `.lmo` gets exactly one mirror; generic profile output remains byte-identical.

Add a two-joint skinned fixture with `MatModel` translation `(3,4,0)`, an asymmetric child translation, and one animated rotation/translation key. Require this literal hierarchy and values:

```text
part_root matrix translation = (3,0,-4)       # G * MatModel * G once
part_root children = [mesh, skeleton roots]
mesh local matrix = identity
mesh vertex data contains no MatModel translation
joint locals/inverse binds/animation = coordinate-mirrored only
```

The fixture asserts literal skinned bounds/root-bone world positions after one part-root application and a second set of deliberately double-baked values that must not appear. Require literal mirrored bind matrices, inverse bind matrices, joint-node transforms, and every animation sample. A malformed skin returns `INVALID_SKIN_DATA` and publishes neither `.gltf` nor `.bin`; a valid skin with non-identity `MatModel` must succeed.

Store the synthetic source values and literal expectations in tracked `tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json`. Add a deterministic test-only converter command:

```bash
./tools/AssetConverter/build/AssetConverter scene-map-fixture \
  --contract tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json \
  --output <run>/synthetic/nonidentity-skinned
```

The command rejects an output inside the real models directory and writes exactly `<run>/synthetic/nonidentity-skinned/nonidentity-skinned.gltf`, `nonidentity-skinned.bin`, and `synthetic-nonidentity-skinned.json`. The manifest is schema v1, `kind="scene-map-synthetic-nonidentity-skinned"`, and contains normalized run-relative paths plus SHA-256 for the tracked contract, glTF, and bin; the coordinate profile; literal part-root/bounds/root-bone/single-apply expectations; and the forbidden double-apply values. It validates the pair and publishes the manifest last by same-directory temp-file flush/fsync, atomic rename, and parent-directory fsync. Two clean producer runs over the same contract must give identical glTF/bin hashes and semantically identical manifests after excluding the run-relative output prefix. Neither the fixture manifest nor its asset paths may appear in real `reference-models.json`, real `scene-assets.json`, the 639/2021/79 census, or Garner placement.

Preserve the full fixed-size material `RsSet` in `LgoMaterial` instead of dropping it. Modern fixtures copy `RenderStateAtom[]`; v0/v1 fixtures convert `RenderStateSet2x8` exactly like `LgoLoader::LoadMtlTexInfoSingle`, including literal `ALPHAFUNC -> GREATER` and `ALPHAREF -> 129` upgrades and the invalid terminator. Add literal parser/resolver/glTF tests for:

```text
opaque:   transp_type=FILTER, opacity=1, alpha test/blend disabled
masked:   FILTER, opacity=1, ALPHATESTENABLE=1,
          ALPHAFUNC=GREATER, ALPHAREF=129
alpha:    FILTER, opacity=0.5 and SRCBLEND=SRCALPHA,
          DESTBLEND=INVSRCALPHA
additive: ADDITIVE, raw/effective transp_type=1/1,
          SRCBLEND=ONE, DESTBLEND=ONE
subtractive: raw transp_type=2, effective transp_type=5,
             SRCBLEND=ZERO, DESTBLEND=INVSRCCOLOR
```

The glTF material carries `extras.corsairsLegacyMaterial` with literal `schemaVersion`, `mode`, `opacity`, `rawTranspType`, `effectiveTranspType`, `alphaTestEnabled`, `alphaRef`, `alphaFunc`, `alphaBlendEnabled`, `srcBlend`, and `destBlend`. Core `alphaMode` is `OPAQUE`, `MASK` with `alphaCutoff=(alphaRef+1)/255`, or `BLEND`; additive uses core `BLEND`, while subtractive uses core `BLEND` only as a transport hint because its authoritative destination-dependent equation comes from extras. Conflicting duplicates, masked+blended state, unsupported alpha funcs/blend pairs, a raw/effective normalization mismatch, and an effective `transp_type` outside the supported `FILTER`/`ADDITIVE`/`SUBTRACTIVE` contract return `UNSUPPORTED_MATERIAL_MODE` and leave no output. No mode may be inferred from decoded texture pixels.

Add a literal real-file resolver fixture for `Client/model/scene/by-bd001.lmo`, object `model1`: its used material has raw `transp_type=2`, explicit `SRCBLEND=ZERO`/`DESTBLEND=INVSRCCOLOR`, must normalize to effective type `5`, and must resolve to `subtractive`. The expected values are literals in the test, not produced by the normalization helper. Add a second literal equation fixture proving `(src,dst) -> dst * (1 - src)` channel-by-channel and rejecting alpha/additive output.

- [ ] **Step 2: Verify RED**

```bash
cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tools/AssetConverter/build \
  --target AssetConverterTests -j2
ctest --test-dir tools/AssetConverter/build -j1 --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_reference_model_set -v
```

Expected failures because the profile, skinned part-root/animation path, five-mode material resolver, full `RsSet`, and reference-model resolver do not exist. A zero-test run or a pass from a pre-existing output is not RED evidence.

- [ ] **Step 3: Implement the complete mirror**

After generic source-to-glTF conversion, apply `G=diag(1,1,-1,1)`:

- positions and normals flip glTF Z;
- the second winding flip cancels the generic one, preserving source index order;
- every resolved `MatModel` and dummy transform gets `G*M*G`;
- tangents, when present, flip xyz and invert handedness w;
- bind matrices, inverse bind matrices, joint-local matrices, and every animation-track matrix get `G*M*G` before decomposition;
- joint indices and weights remain unchanged;
- `GltfSkeletonWriter` receives the same coordinate profile so generic character/equipment output remains byte-identical.

Emit one `part_root` for every part, including skinned parts. `part_root` alone owns converted `MatModel`; the mesh node is identity, and skeleton roots/dummies are children of the same part root. Do not pre-multiply `MatModel` into positions, bind/inverse-bind matrices, joint locals, or animation samples. Add an invariant that rejects a non-identity skinned `MatModel` unless the serialized graph contains exactly one such root.

Parser version adapters populate one canonical `RenderStates` array first; no later stage knows the disk version. They preserve the disk value as `RawTranspType` and apply the original `LgoLoader` normalization exactly once: raw `1 -> 1`, raw `2 -> 5`, and every other raw value remains numerically unchanged before support validation. `ResolveLegacyMaterial` consumes `EffectiveTranspType`, canonicalizes the atoms, and rejects conflicting duplicate states. It follows original-client state precedence: `ADDITIVE(1)` requires effective `ONE/ONE`; `SUBTRACTIVE(5)` requires effective `ZERO/INVSRCCOLOR`; `FILTER(0)` with an explicit blend requires exactly `SRCALPHA/INVSRCALPHA` and is `alpha`; `FILTER` with no explicit blend and opacity below one is also `alpha`; alpha-test-only `FILTER` with `GREATER` is `masked`; the remaining disabled-test/disabled-blend `FILTER` is `opaque`. Any other effective `transp_type`, blend pair, alpha function, simultaneous alpha-test/blend, or raw/effective mismatch is fatal. Store the resolved metadata, including both transparency fields, in each glTF material extras object and in `scene-assets.json`; consumers never recompute it.

Add CLI `--profile generic|scene-map`, `--required-models <reference-models.json>`, and the separate `scene-map-fixture` subcommand above. SceneMap output path is separate from generic and synthetic assets. Write one real conversion attempt into a unique run directory, validate every `.gltf`/`.bin` pair, and publish real `scene-assets.json` last by atomic temp-file rename. Each real part record includes `partRootMatrix`, `assetKind`, material metadata, and exactly one animation record (`path/name/duration/loop`) or explicit `null` reference pose. The manifest carries the required-model manifest path/SHA and proves every required real model/part was emitted. Consumers use only files listed by the relevant real or synthetic manifest; never attempt a multi-file overwrite in an existing directory. Embedded animation is not silently dropped: valid data is mirrored and exported; malformed or unsupported data fails the owning conversion without publishing its manifest.

- [ ] **Step 4: Verify GREEN and real conversion**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests -j2
ctest --test-dir tools/AssetConverter/build -j1 --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_reference_model_set -v
task4RunDir="<exact TASK4_RUN_DIR recorded by Task 4 GREEN>"
test -f "$task4RunDir/garner.objects.json"
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/build_scene_reference_model_set.py \
  --source-manifest "$task4RunDir/garner.objects.json" \
  --database databases/gamedata.sqlite \
  --model-root Client/model/scene \
  --output "$task4RunDir/reference-models.json"
./tools/AssetConverter/build/AssetConverter \
  Client/model/scene "$task4RunDir/models" \
  --textures Client/texture/scene \
  --profile scene-map \
  --required-models "$task4RunDir/reference-models.json"
./tools/AssetConverter/build/AssetConverter scene-map-fixture \
  --contract tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json \
  --output "$task4RunDir/synthetic/nonidentity-skinned"
```

Run the build, CTest, Python resolver test, real corpus conversion, and validation sequentially; never overlap them. Compilation remains `-j2`, CTest remains `-j1`, and this task may not raise the global thermal/concurrency caps.

The real conversion is a gate, not an example. Its mandatory required-model set comes only from Task 4's schema-v2 records and the tracked DB resolver above; a literal list (including a remembered skeletal-ID list) or success from scanning all files cannot close this gate. Resolve all 1634 reference anchors through that set, require each contributing model/part exactly once, and allow a wider all-corpus conversion only as additional coverage. No `INVALID_SKIN_DATA`, generic-profile fallback, missing `.bin`, stale required-model hash, or unlisted reference input is allowed.

The all-corpus audit records literal baselines `lmoCount=639`, `partCount=2021`, `skinnedPartCount=79`, checks every one of the 79 real skinned parts exactly once, and requires the real `skinnedNonIdentityMatModel` census to equal `[]`. Empty is valid. For every real skinned part, its serialized graph must still contain exactly one `part_root`, an identity mesh node, mirrored skin/bind/animation data, and no baked `MatModel` in vertices or actor transform. The non-identity/double-bake gate is instead the separate tracked synthetic fixture from Step 1; Task 7 imports that fixture headlessly and checks its literal bounds/root-bone transforms. Its output and records are kept outside real `scene-assets.json`, `reference-models.json`, and real censuses.

The real material census records exact counts for all five modes and requires every used material to belong to exactly one. "Used material" means one unique `(lmoPath, partIndex, sourceMaterialIndex)` referenced by at least one emitted mesh primitive/subset; an unused material-table slot is not counted, and repeated triangles do not multiply the count. The census separately requires 48 used raw-type-2 materials across exactly 16 distinct `.lmo` inputs, each preserved as `rawTranspType=2`, normalized to `effectiveTranspType=5`, and resolved as `subtractive`; `by-bd001/model1` is the literal named member. Unsupported modes abort before `scene-assets.json` is renamed. Garner reference placement need not contain every mode, so five-mode capability is protected by the literal synthetic fixtures and the full-corpus census rather than a nonzero per-reference-mode assumption.

- [ ] **Step 5: Commit**

```bash
git add \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/LgoParser.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/GltfWriter.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h \
  tools/AssetConverter/src/LgoParser.cpp \
  tools/AssetConverter/src/GltfWriter.cpp \
  tools/AssetConverter/src/GltfSkeletonWriter.cpp \
  tools/AssetConverter/src/Main.cpp \
  tools/AssetConverter/tests/TestGltfWriter.cpp \
  tools/AssetConverter/tests/TestGltfSkeletonWriter.cpp \
  tools/AssetConverter/tests/TestLgoParser.cpp \
  tools/AssetConverter/tests/TestLmoParser.cpp \
  tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json \
  CorsairsUE/Scripts/build_scene_reference_model_set.py \
  CorsairsUE/Scripts/tests/test_scene_reference_model_set.py
git commit -m "feat(converter): mirror scene map assets"
```

---

### Task 4: Publish a source manifest with terrain and reference-set facts

**Task brief:** Join stable scene records to terrain facts without resolving assets. Input is Task 1 selection, independently supplied source context, and Task 2 tile source; output is atomic schema-v2 source truth for every type-0/type-1 record. Terrain/type/context/count failures commit nothing and restore any prior destination. A persistent filesystem rollback failure with a prior destination preserves its exact bytes/mode in a verified recovery backup; without a prior destination, recovery creates no backup and reports every unresolved cleanup path.

**Files:**

- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapWriter.h`
- Modify: `tools/AssetConverter/src/MapWriter.cpp`
- Modify: `tools/AssetConverter/src/Main.cpp`
- Modify: `tools/AssetConverter/tests/TestSceneParity.cpp`

**Interface:**

```cpp
enum class SceneManifestStatus {
    OK,
    WRITE_FAILED,
    UNKNOWN_OBJECT_TYPE,
    INVALID_SOURCE_CONTEXT,
    TERRAIN_READ_FAILED,
    COUNT_MISMATCH,
    RECOVERY_REQUIRED,
};

struct SceneManifestStats {
    std::uint64_t SourceRecordCount{0};
    std::uint64_t SceneModelCount{0};
    std::uint64_t DeferredEffectCount{0};
    std::uint64_t ReferenceObjectCount{0};
    std::map<std::uint8_t, std::uint64_t> ReferenceIslandCounts;
};

struct SceneManifestSourceContext {
    SceneFileHeader ObjectHeader{};
    std::string SourceMapSha256;
    std::string SourceObjectSha256;
    std::uint64_t ExpectedSourceRecordCount{0};
    std::uint64_t ExpectedSceneModelCount{0};
    std::uint64_t ExpectedDeferredEffectCount{0};
    std::uint64_t ExpectedReferenceObjectCount{0};
    std::map<std::uint8_t, std::uint64_t> ExpectedReferenceIslandCounts;
};

SceneManifestStatus WriteSceneSourceManifest(
    const SceneSelection& selection,
    IMapTileSource& terrain,
    const SceneManifestSourceContext& context,
    const std::filesystem::path& basePath,
    SceneManifestStats& stats,
    std::string& detail);
```

`SceneManifestSourceContext` is explicit input, not state recovered by the writer. The `scene-manifest` CLI constructs it from the exact command-line inputs through a data path separate from `BuildSceneSelection`: it retains the independently parsed object header as `ObjectHeader`, hashes those exact object bytes as `SourceObjectSha256`, and streams the exact `.map` bytes through SHA-256 as `SourceMapSha256` without `ReadWholeFile` or a full-map tile allocation. The writer requires every field of `context.ObjectHeader` to equal `selection.SourceHeader`; it never derives either header from per-record maxima. Tests mutate each header field on either side and require `INVALID_SOURCE_CONTEXT`. Task 1 does not hash files. Generic unit fixtures supply their own header, hashes, and expected counts; the real Garner command supplies literal expectations `50017` total records, `46991` scene models, `3026` deferred effects, `1634` reference objects, and islands `{0:3, 1:1625, 2:6}`.

Add CLI:

```bash
./tools/AssetConverter/build/AssetConverter scene-manifest \
  Client/map/garner.map \
  Client/map/garner.obj \
  artifacts/scene-parity/runs/<task4Run>/garner
```

- [ ] **Step 1: Add RED JSON and count tests**

Require the top-level schema and every source record:

```json
{
  "schemaVersion": 2,
  "sourceMapSha256": "<sha256>",
  "sourceObjectSha256": "<sha256>",
  "sectionCntX": 512,
  "sectionCntY": 512,
  "sectionWidth": 8,
  "sectionHeight": 8,
  "stats": {
    "sourceRecordCount": 50017,
    "sceneModelCount": 46991,
    "deferredEffectCount": 3026,
    "referenceObjectCount": 1634,
    "referenceIslandCounts": {"0": 3, "1": 1625, "2": 6}
  },
  "records": [
    {
      "sourceKey": {
        "sectionIndex": 1,
        "slotIndex": 0,
        "byteOffset": 100
      },
      "modelId": 1,
      "type": 0,
      "disposition": "scene-model",
      "x": 223325,
      "y": 278475,
      "heightOff": 0,
      "sourceYawDegrees": 90,
      "sourceScaleDiagnostic": 37,
      "surfaceHeightCm": 60,
      "zCm": 60,
      "terrainSectionPresent": true,
      "island": 1,
      "tileColor565": 31727,
      "inReferenceSet": true
    }
  ]
}
```

Type-0 records use `"disposition":"scene-model"` and may enter the reference set. Type-1 records use `"disposition":"deferred-effect"`, retain the same source-key, position, terrain, and transform facts, always have `inReferenceSet=false`, and never carry or resolve a scene asset. Merge the two `SceneSelection` vectors back into stable source-key order when writing; source-record count and the 3026 Garner deferred effects must be auditable from this one manifest.

Require stats fields:

```text
sourceRecordCount
sceneModelCount
deferredEffectCount
referenceObjectCount
referenceIslandCounts
```

Generic fixtures pass expectations matching their synthetic partition. Real Garner must report and gate exact stats: `sourceRecordCount=50017`, `sceneModelCount=46991`, `deferredEffectCount=3026`, `referenceObjectCount=1634`, island1 1625, island2 6, and absent/default island0 3. A mismatch in any scalar count, island key, or island count returns `COUNT_MISMATCH` and publishes nothing.

Require `context.ObjectHeader` to equal the independently parser-owned `selection.SourceHeader` field-for-field and the terrain grid to cover those complete-section extents. Both hashes must be lowercase 64-character hexadecimal values. A missing/malformed hash, inconsistent header, duplicate source key, invalid partition member, terrain error, or mismatched expectation fails before publication. Two independent writes over the same exact inputs and expectations must produce byte-identical JSON and the same SHA-256. Task 4's GREEN publishes one of those identical files durably inside `artifacts/scene-parity/runs/<task4Run>/garner.objects.json` and records that exact absolute run directory in its task report. Task 3 reads that exact file in place; no `artifacts/maps` alias, regeneration, directory scan, newest-run lookup, or copied manifest is an accepted handoff.

Add explicit RED atomic fault fixtures through a test-only filesystem fault adapter. It must expose named points for `BACKUP_PARENT_FSYNC`, `AFTER_REPLACE`, `POST_REPLACE_PARENT_FSYNC`, `ROLLBACK_REPLACE`, `ROLLBACK_PARENT_FSYNC`, `ROLLBACK_VERIFY`, `NO_PRIOR_REMOVE`, `NO_PRIOR_PARENT_FSYNC`, and `NO_PRIOR_VERIFY_ABSENT`. A dedicated prior-destination subcase injects one-shot `BACKUP_PARENT_FSYNC`, requires the literal old bytes and non-default mode to remain at the destination, proves that destination replace was never called, and expects ordinary `WRITE_FAILED` plus successful temp/backup cleanup. The persistent-rollback test starts from the same prior destination and combines one-shot `AFTER_REPLACE` with, in separate subcases, persistent `ROLLBACK_REPLACE`, `ROLLBACK_PARENT_FSYNC`, or `ROLLBACK_VERIFY`. Every persistent subcase requires `RECOVERY_REQUIRED`, an existing same-directory backup with the exact old bytes and mode, and `detail` containing that exact recovery path. It must never return `WRITE_FAILED` or delete/overwrite the verified backup.

Add a distinct no-prior-destination fixture. No backup file may be created in any subcase. On an ordinary failure, cleanup removes whichever publication artifacts exist, fsyncs the parent, and verifies both destination and temp absence: a failure before replace therefore removes the temp while preserving destination absence, and a failure after replace removes the new destination. Exercise `NO_PRIOR_REMOVE`, `NO_PRIOR_PARENT_FSYNC`, and `NO_PRIOR_VERIFY_ABSENT` against both the applicable pre-replace cleanup and post-replace cleanup. Any persistent cleanup failure returns `RECOVERY_REQUIRED`; `detail` names every unexpected destination/temp path plus exact cleanup/retry instructions and includes literal `priorBackup=null`.

- [ ] **Step 2: Verify RED**

```bash
cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tools/AssetConverter/build \
  --target AssetConverterTests -j2
ctest --test-dir tools/AssetConverter/build \
  -j1 --output-on-failure
```

Expected failures because the context, schema-v2 fields, generic terrain error channel, atomic writer, and production command do not exist. Run these commands sequentially; do not overlap compilation or CTest and do not raise the global `-j2`/`-j1` limits.

- [ ] **Step 3: Implement atomic manifest publication**

The top-level document has the literal fields `schemaVersion: 2`, `sourceMapSha256`, `sourceObjectSha256`, `sectionCntX`, `sectionCntY`, `sectionWidth`, `sectionHeight`, `stats`, and ordered `records`. Generate and validate all records before opening a publication temp. After every terrain query, inspect `IMapTileSource::LastError()`; a nonempty sticky error is `TERRAIN_READ_FAILED`, even though the safe tile value may look like an absent section.

Write a unique same-directory temp, flush and fsync its bytes, close it, then reparse and validate the complete schema/stats/order/hashes from those temp bytes. If `<base>.objects.json` already exists, first copy its exact bytes and mode to a unique same-directory backup, flush/fsync the backup, verify its SHA-256 against the destination, **and fsync the parent directory so the verified backup directory entry is durable before destination replace**. Failure at this `BACKUP_PARENT_FSYNC` point leaves the original destination untouched, removes temp/backup when cleanup succeeds, and returns `WRITE_FAILED`; replace is forbidden before this fsync succeeds. Atomically replace the destination with the temp, reparse and hash the published destination, then fsync the parent directory. Only after those steps succeed is the new publication committed. Backup cleanup is post-commit housekeeping: normal success removes it and fsyncs the directory again, while cleanup failure retains the verified backup and reports its path without downgrading the already durable publication.

Inject failures after temp serialization, after temp fsync, during backup parent-directory fsync, immediately before replace, immediately after replace, and on the first post-replace parent-directory fsync. Every ordinary pre-commit failure restores the prior state. With a prior destination, keep the verified backup immutable, clone its exact bytes and mode into a separate same-directory rollback temp, flush/fsync that fully prepared rollback temp, atomically replace the destination from it, fsync the directory, and verify exact prior bytes, mode, and SHA; only then remove backup/temp artifacts. This ensures rollback-replace/fsync/verification failure still leaves the old bytes and mode in the backup named by `detail`. A one-shot injected publication fsync failure must allow rollback operations. If any rollback replace/fsync/verification persistently fails, return `RECOVERY_REQUIRED`, retain the verified backup, and never return an ordinary error.

When no prior destination exists, never create a backup. Every ordinary pre- or post-replace failure removes every extant temp/destination artifact, fsyncs the parent, and verifies their absence. If artifact removal, cleanup fsync, or absence verification persistently fails, return `RECOVERY_REQUIRED` with every extant/uncertain destination/temp path and exact cleanup/retry instructions; `detail` explicitly states `priorBackup=null`. Thus every ordinary failure restores prior bytes/existence, while every recovery-required state preserves or names all available recovery evidence without masquerading as success. Unknown type, malformed source context, terrain read failure, any expected-count mismatch, serialization/reparse failure, or replace/fsync failure cannot expose a partial or unverified manifest as success.

- [ ] **Step 4: Verify GREEN and commit**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests -j2
ctest --test-dir tools/AssetConverter/build \
  -j1 --output-on-failure

mkdir -p "$PWD/artifacts/scene-parity/runs"
task4RunDir="$(mktemp -d "$PWD/artifacts/scene-parity/runs/task4.XXXXXXXX")"
task4ProbeDir="$(mktemp -d)"
./tools/AssetConverter/build/AssetConverter scene-manifest \
  Client/map/garner.map Client/map/garner.obj "$task4RunDir/garner"
./tools/AssetConverter/build/AssetConverter scene-manifest \
  Client/map/garner.map Client/map/garner.obj "$task4ProbeDir/garner"
cmp "$task4RunDir/garner.objects.json" "$task4ProbeDir/garner.objects.json"

mapSha="$(shasum -a 256 Client/map/garner.map | awk '{print $1}')"
objectSha="$(shasum -a 256 Client/map/garner.obj | awk '{print $1}')"
storedMapSha="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["sourceMapSha256"])' "$task4RunDir/garner.objects.json")"
storedObjectSha="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["sourceObjectSha256"])' "$task4RunDir/garner.objects.json")"
test "$storedMapSha" = "$mapSha"
test "$storedObjectSha" = "$objectSha"
shasum -a 256 \
  "$task4RunDir/garner.objects.json" \
  "$task4ProbeDir/garner.objects.json"
printf 'TASK4_RUN_DIR=%s\n' "$task4RunDir"
```

Expected: build and CTest pass; both real commands exit zero; `cmp` succeeds; both manifest SHA-256 values are identical; both stored source hashes equal independent `shasum -a 256` results; the manifest contains the exact schema/counts above. Record the printed absolute `TASK4_RUN_DIR` in the Task 4 report and retain that run directory for Task 3. Execute every command in order under the global thermal limits. Do not overlap either converter run with compilation or CTest.

```bash
git add \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapWriter.h \
  tools/AssetConverter/src/MapWriter.cpp \
  tools/AssetConverter/src/Main.cpp \
  tools/AssetConverter/tests/TestSceneParity.cpp
git commit -m "feat(converter): write scene parity manifest"
```

---

### Task 5: Build the scene catalog with original lighting flags

**Task brief:** Resolve reference-relevant type-0 mesh parts plus every all-Garner placed type-3 light source, while keeping mesh placement reference-scoped. Inputs are the tracked DB, the complete source manifest, Task 3 `reference-models.json`, and Task 3 `scene-assets.json`; output is one atomically published deterministic dual-scope catalog. A missing required light-model row, missing reference part, SHA mismatch, generic asset, incomplete animation/material DTO, or unsupported material metadata is fatal and cannot publish a partial file.

**Files:**

- Create: `CorsairsUE/Scripts/build_scene_catalog.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_catalog.py`

**Interfaces:**

```python
import math


def parse_rgb_triplet(value: str) -> tuple[int, int, int]:
    parts = tuple(int(part.strip()) for part in value.split(","))
    if len(parts) != 3:
        raise ValueError("expected three RGB components")
    return parts

def parse_packed_argb(value: int) -> tuple[float, float, float, float]:
    unsigned = value & 0xFFFFFFFF
    return (
        ((unsigned >> 16) & 0xFF) / 255.0,
        ((unsigned >> 8) & 0xFF) / 255.0,
        (unsigned & 0xFF) / 255.0,
        ((unsigned >> 24) & 0xFF) / 255.0,
    )

def parse_direction(value: str) -> tuple[float, float, float]:
    x, y, z = (float(part.strip()) for part in value.split(","))
    length = math.sqrt(x * x + y * y + z * z)
    if length == 0.0:
        raise ValueError("light direction must be non-zero")
    return x / length, -y / length, z / length
```

The catalog entry point is:

```python
def build_catalog(
    db_path: str,
    gltf_root: str,
    source_manifest_path: str,
    reference_model_set_path: str,
    content_root: str = "/Game/SceneParity",
) -> dict: ...

def write_catalog_atomic(catalog: dict, output_path: str) -> None: ...
```

Its exact top-level keys are `schemaVersion`, `sourceMapSha256`, `referenceModelSetSha256`, `sceneAssetsSha256`, `materialModeCensus`, `models`, `pointSources`, `areas`, and `animatedLights`. `materialModeCensus` preserves the five exact real mode counts plus the raw-2 count and `.lmo` count from Task 3; later consumers compare it rather than recounting with a new resolver. The scopes are deliberately different:

- `models` contains part/asset metadata only for distinct type-0 model IDs in the 1634-anchor reference set;
- ordered `pointSources` contains one entry for **every** all-map type-0 source record whose `scene_objects.type == 3`, including records outside the 80 m reference set. It is joined in source-manifest order and contains `sourceKey`, `sourceOrder`, `modelId`, source-space `positionCm: [x,y,zCm]`, literal `type: 3`, `point_color`, `range`, normalized `attenuation0/1/2`, and `anim_ctrl_id`. It carries no `parts` and therefore cannot widen scene mesh placement.

The source manifest is required both to enforce completeness of the 1634 reference anchors and to enumerate the all-Garner light placements. The real-data gate independently queries the tracked DB for type-3 model rows, joins them to **all** ordered Garner type-0 records, and requires every placed row to appear exactly once in `pointSources`; the current type-3 model-ID set is the literal `{400,401,402,403,404,405,406,407,408}` and all nine IDs must occur. A missing/duplicate source key, absent required DB row, wrong DB type, inactive/malformed required light field, or source/catalog SHA mismatch is fatal. A lighting-only entry never triggers SceneMap part conversion unless that same model ID is independently present in the reference set.

- [ ] **Step 1: Add RED catalog tests**

Require every model to carry:

```text
type
enable_env_light
enable_point_light
shade_flag
size_flag
point_color
env_color
range
attenuation0
attenuation1
attenuation2
anim_ctrl_id
data_name
parts[]: assetPath + assetKind(static|skeletal) + partRootMatrix
         + animation { path, name, duration, loop } | null
         + materials[] { slot, sourceMaterialIndex, schemaVersion,
                         mode, opacity,
                         rawTranspType, effectiveTranspType,
                         alphaTestEnabled, alphaRef, alphaFunc,
                         alphaBlendEnabled,
                         srcBlend, destBlend }
```

Static DB objects map their legacy linear attenuation to `(attenuation0,attenuation1,attenuation2)=(0,value,0)`. Animated-light keyframes retain all three original attenuation coefficients independently.

Literal DB gates:

- ID 22 Bell Tower flags are `0/0/0` and every part is under `/Game/SceneParity`;
- IDs 314 and 323 flags are `1/0/0`;
- area 1 packed `-9268069` equals `0xFF72949B`;
- source direction `(-1,-1,-1)` becomes normalized UE direction `(-1,+1,-1)/sqrt(3)`;
- an unresolved model inside the reference set is fatal;
- type-1 source records do not invoke model lookup even when their numeric ID equals a valid scene model;
- every skeletal model reached by the Task 4 `type=0 && inReferenceSet` join resolves to its required SceneMap assets and no generic `/Game/All` path; the test derives and freezes the expected ordered IDs from its source-manifest fixture rather than embedding a production literal list;
- every skeletal part has either one deterministic SceneMap animation object whose `path`, `name`, `duration`, and `loop` equal Task 3 byte-for-byte or an explicit `null` meaning reference pose; static parts require `animation=null` and cannot carry an animation object;
- every real skeletal part preserves its literal (currently identity) `partRootMatrix` from real `scene-assets.json`; an isolated synthetic catalog-unit fixture also proves a non-identity matrix is copied unchanged, but that fixture/path is forbidden from the published real catalog and is reserved for Task 7's separate headless import;
- every Task 3 material DTO field survives unchanged into the catalog: `slot`, `sourceMaterialIndex`, `schemaVersion`, `mode`, `opacity`, `rawTranspType`, `effectiveTranspType`, `alphaTestEnabled`, `alphaRef`, `alphaFunc`, `alphaBlendEnabled`, `srcBlend`, and `destBlend`; subtractive retains literal raw/effective `2/5` and `ZERO/INVSRCCOLOR`. Missing fields, an unknown mode, a raw/effective mismatch, a duplicate slot, or disagreement between glTF extras, `scene-assets.json`, and the full-corpus five-mode census is fatal;
- unused sentinel point fields on legacy-unlit ID 22 are preserved diagnostically and not validated as active point-light inputs.
- an out-of-reference type-0 fixture for model 406 and another for model 407 both appear in ordered `pointSources`, while neither causes a `models`/parts entry; deleting either DB row, changing its type from 3, dropping either source, or reordering equal source keys is fatal;
- the real catalog contains every all-map occurrence of type-3 IDs 400 through 408, and an independent source-manifest/SQL join produces the identical ordered `(sourceKey,modelId)` list. This is a placement-count gate, not merely a nine-ID membership check.

Add publication failure injection at three boundaries: after temp serialization, after temp file fsync, and immediately before `os.replace`. With no prior output, every injected failure leaves no output and no temp file. With a valid prior output, every injected failure preserves its exact bytes. Malformed/incomplete animation or material DTO validation fails before rename. A successful write must replace the prior file atomically and the test reopens/parses the final bytes.

- [ ] **Step 2: Verify RED**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_catalog -v
```

Expected import failure.

- [ ] **Step 3: Implement deterministic catalog**

Read tracked `databases/gamedata.sqlite`, validate source-manifest schema v2 and SHA, require Task 3 `reference-models.json` to be the identical ordered distinct selection, validate the Task 3 `scene-assets.json` required-input hash, and make two deterministic passes. First, resolve part files only for the reference-set type-0 IDs from the atomically published SceneMap asset manifest. Second, read all DB `scene_objects` rows with `type=3`, validate the literal real set 400 through 408, and join those rows to every ordered all-map type-0 source record to publish `pointSources`; missing required rows/occurrences are fatal. Preserve source order for point placements and DB order for light keyframes, sort keyed JSON objects numerically, and fail on any unresolved reference-set model. Copy `partRootMatrix`, the complete animation object/null, and every per-slot Task 3 material field verbatim; do not reopen texture pixels, repeat `2 -> 5` normalization, or derive blend mode from `opacity` here. Never use a type-1 record to query `scene_objects`, and never resolve mesh parts merely because a record is a point source.

Validate the complete in-memory DTO before opening a temp output. `write_catalog_atomic` serializes deterministically to a unique same-directory temp file, flushes and `os.fsync`s it, reparses and validates the temp bytes, calls `os.replace`, then fsyncs the parent directory. Any exception before replace removes the temp and leaves an existing destination unchanged; when no destination existed, failure leaves no output. Publish no side file that a consumer could mistake for a catalog.

- [ ] **Step 4: Verify GREEN and commit**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_catalog -v
git add \
  CorsairsUE/Scripts/build_scene_catalog.py \
  CorsairsUE/Scripts/tests/test_scene_catalog.py
git commit -m "feat(ue): build scene parity catalog"
```

---

### Task 6: Resolve legacy ambient, directional, shade, and point lights

**Task brief:** Reduce source/catalog lighting to a deterministic per-part payload at capture tick 120. Inputs are Tasks 4/5 manifests and all Garner point-light sources; output is ordered schema-v2 placement with a complete 33-float logical payload per part; SHA, area inheritance, point selection, and animation errors are fatal.

**Files:**

- Create: `CorsairsUE/Scripts/scene_lighting.py`
- Create: `CorsairsUE/Scripts/build_scene_placement_manifest.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_lighting.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_placement_manifest.py`

**Interfaces:**

- `evaluate_animated_light(keyframes: list[dict], capture_tick: int = 120, capture_hz: int = 30) -> dict`
- `select_legacy_point_lights(anchor: tuple[float, float, float], point_sources: list[dict]) -> list[dict]`
- `resolve_object_lighting(obj: dict, catalog: dict, area: dict | None, point_sources: list[dict], capture_tick: int = 120) -> dict`
- `build_placement_manifest(source_manifest: dict, catalog: dict) -> dict`

- [ ] **Step 1: Add RED point-selection and animation tests**

Source order A(d²=100), B(d²=25), C(d²=25), D(d²=400) must select B then C. Equal distance preserves earlier source order; there is no distance cutoff. Add an all-map catalog fixture in which reference anchors are near out-of-reference light models 406 and 407: both must participate and win when closest. Removing either `pointSources` entry is RED even though both anchors remain outside the reference set and `models` remains unchanged.

Keyframes:

```text
frame 0: rgb=(0,0,0), range=10, attenuation=(1.0,0.2,0.01)
frame 10: rgb=(100,200,50), range=20, attenuation=(2.0,0.6,0.03)
frame 5: rgb=(50,100,25), range=15, attenuation=(1.5,0.4,0.02)
```

Loop over `0..lastFrame` using the legacy `fmod(frame,lastFrame+1)` rule. Add an independent literal expected result for capture tick 120 that verifies interpolation of RGB, range, and all three attenuation coefficients.

- [ ] **Step 2: Add RED lighting-mode tests**

Require:

- flags `0/0/0` -> `legacy-unlit`, with no area/point input;
- env=1 -> area directional/ambient;
- point=1 -> exactly two selected sources;
- shade=1,size=1 -> tileRGB565 multiplied by area env;
- shade=1,size=0 -> tileRGB565 multiplied by white;
- shade=0 ignores tile tint;
- absent island0 -> `legacy-inherited-outside-capture`, never area1.

The placement manifest has top-level `schemaVersion: 2`, the same `sourceMapSha256`, `captureTick: 120`, `captureHz: 30`, and ordered `instances`. Every instance stores source key, source type, part index, asset, expected transform, resolved lighting, point source keys/d²/range/attenuation0/attenuation1/attenuation2. The builder rejects a catalog/source SHA mismatch.

- [ ] **Step 3: Verify RED**

Run the two focused Python test modules; expected failures are the unimplemented functions.

- [ ] **Step 4: Implement and verify GREEN**

Consume `catalog["pointSources"]` directly; do not reconstruct lights from reference-scoped `catalog["models"]`. Require it to equal the independent all-Garner source/DB type-3 join, then select from every entry, not only the 80 m set. Use stable `sourceOrder`/source-key order, require unique complete keys and real IDs 400 through 408, and fail rather than silently omitting an unresolved required light model.

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_lighting \
  CorsairsUE.Scripts.tests.test_scene_placement_manifest -v
```

- [ ] **Step 5: Commit**

```bash
git add \
  CorsairsUE/Scripts/scene_lighting.py \
  CorsairsUE/Scripts/build_scene_placement_manifest.py \
  CorsairsUE/Scripts/tests/test_scene_lighting.py \
  CorsairsUE/Scripts/tests/test_scene_placement_manifest.py
git commit -m "feat(ue): resolve legacy scene lighting"
```

---

### Task 7: Place mirrored scene assets deterministically in Unreal

**Task brief:** Import only immutable SceneMap assets and materialize the placement manifest with a transport-correct lighting payload for both mesh kinds. Inputs are schema-v2 source/catalog/placement manifests; output is an unsaved, tagged parity population in the already loaded level; wrong asset kind, material parent/mode, payload length/transport, or transform is fatal.

**Dependencies:** Task 3's real `scene-assets.json`, Task 3's explicit run-local `<run>/synthetic/nonidentity-skinned/synthetic-nonidentity-skinned.json`, and Task 6 placement. Task 7 never discovers a fixture by directory scan or test temp state: its importer requires the manifest path as an explicit CLI argument, validates all recorded SHA-256 values, and imports only under `/Game/SceneParityTests/<runId>`. The synthetic fixture never participates in the real catalog, population, or censuses.

**Files:**

- Modify: `CorsairsUE/Source/CorsairsImport/Public/SceneManifest.h`
- Modify: `CorsairsUE/Source/CorsairsImport/Private/SceneManifest.cpp`
- Create: `CorsairsUE/Source/CorsairsImport/Private/Tests/SceneManifestTests.cpp`
- Create: `CorsairsUE/Scripts/scene_parity_placement.py`
- Create: `CorsairsUE/Scripts/place_scene_parity.py`
- Create: `CorsairsUE/Scripts/setup_scene_parity_material.py`
- Create: `CorsairsUE/Scripts/check_scene_parity_materials.py`
- Create: `CorsairsUE/Scripts/import_scene_parity_test_fixture.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_parity_placement.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_parity_test_fixture.py`

**C++ schema-v2 fields:**

```cpp
// FCorsairsSceneManifest
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 SchemaVersion = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
FString SourceMapSha256;

// FCorsairsPlacedObject
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 SourceSectionIndex = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 SourceSlotIndex = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int64 SourceByteOffset = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
float SurfaceHeightCm = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
bool TerrainSectionPresent = false;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 Island = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
bool InReferenceSet = false;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 SourceYawDegrees = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 SourceScaleDiagnostic = 0;
UPROPERTY(BlueprintReadOnly, Category="Corsairs")
int32 TileColor565 = 0;
```

Remove the legacy `Yaw` and `Scale` members rather than retaining a second transform representation. Keep the existing reflected map dimensions and `Objects` array, but load them from the v2 top-level dimension fields and `records`. The loader requires `schemaVersion == 2`, a 64-character hexadecimal `sourceMapSha256`, every source-key/terrain/transform field including type, height offset, tile color, yaw and diagnostic scale, and parses the nested `sourceKey` object into the flat reflected fields. It rejects legacy/missing/unknown schemas with a precise error and publishes no partially filled `OutManifest`. It never supplies silent zero defaults for required v2 values.

**Transform contract:**

```cpp
static FVector GetObjectLocation(
    const FCorsairsPlacedObject& Object,
    float UnitsPerTile = 100.0f);

static FRotator GetObjectRotation(
    const FCorsairsPlacedObject& Object);

static bool SetSkeletalLightingPayload(
    UPrimitiveComponent* Component,
    const TArray<float>& Values,
    FString& OutError);

static bool GetSkeletalLightingPayload(
    const UPrimitiveComponent* Component,
    TArray<float>& OutValues,
    FString& OutError);
```

Location is `(X, -Y, SurfaceHeightCm + HeightOff * UnitsPerTile/100)`. Rotation yaw is `FMath::UnwindDegrees(180.0f - SourceYawDegrees)`.
The payload helpers require a `USkeletalMeshComponent` and exactly 33 finite values, write/read `UPrimitiveComponent::CustomPrimitiveData` indices `0..32`, clear `OutValues` on failure, and never create a dynamic material. They give Unreal Python and the runtime exporter one tested transport boundary instead of depending on version-specific reflection of `FCustomPrimitiveData`.

- [ ] **Step 1: Add RED C++ transform tests**

Require:

```text
surface 60, heightOff 0 -> Z 60
source yaw 0 -> UE 180, forward (-1,0,0)
source yaw 90 -> UE 90, forward (0,1,0)
source yaw -180 -> UE 0, forward (1,0,0)
source yaw 270 -> UE -90, forward (0,-1,0)
source scale diagnostic never changes instance scale (1,1,1)
```

Add the literal asymmetric imported-world positions from the approved spec and a C++ round-trip of all 33 skeletal CPD values:

```text
A_world=(1400,-1700,300)
B_world=(1400,-1500,300)
C_world=(1500,-1700,300)
```

- [ ] **Step 2: Add RED Python grouping tests**

Instance order is source key then part index. Mapping is exactly:

```text
static:
  sourceKey + partIndex
    -> actorLabel + componentName + instanceIndex
skeletal:
  sourceKey + partIndex
    -> tagged skeletalActorLabel
```

Require stable mapping across two runs, positive scale, and rejection of type 1 or generic `/Game/All` assets. A skeletal catalog part must never enter an HISM group; a static part must never spawn a skeletal actor. Every real skinned part selected by the Task 4-derived reference-model set must produce one tagged skeletal mapping; no literal ID list substitutes for this join. Actor placement applies only the anchor transform; the imported part owns `partRootMatrix`. All real skinned parts are checked exactly once even though the current real nonidentity census is `[]`. Separately, import the tracked synthetic skinned non-identity fixture from the explicit manifest argument and fail on manifest/asset SHA drift, actor-side part-root application, imported identity, or the deliberately double-baked bounds/root-bone values. Skeletal animation is deterministic: explicit `null` selects reference pose; otherwise select the catalog animation object by exact `path` and `name`, set single-node mode, apply its `loop`, seek to `(120/30) mod animation.duration`, and pause. Tests reject another path/name, loop flag, playing state, duration drift, or playback-time error above one source frame.

- [ ] **Step 3: Define and test the material payload**

Create the shared unlit SceneParity material graph with one exactly defined 33-float lighting payload:

```text
0 mode
1 envEnabled
2 pointEnabled
3 shadeEnabled
4..6 resolvedAmbientRGB
7..9 ueLightDirection
10..12 directionalColor
13..15 point1OffsetCm
16..18 point1RGB
19 point1RangeCm
20 point1Attenuation0
21 point1Attenuation1
22 point1Attenuation2
23..25 point2OffsetCm
26..28 point2RGB
29 point2RangeCm
30 point2Attenuation0
31 point2Attenuation1
32 point2Attenuation2
```

Static HISM instances set `num_custom_data_floats=33` and store index `i` as `PerInstanceCustomData(i)`. Each `USkeletalMeshComponent` stores the same index `i` through `SetCustomPrimitiveDataFloat(i, value)` and the skeletal parent reads `CustomPrimitiveData(i)`; dynamic-material scalar/vector parameters are forbidden as a second transport. Both paths consume the one `encode_lighting_payload` result. Pure tests encode/decode HISM and skeletal CPD value-for-value, including `0.0`, negative light offsets, and all attenuation slots, and reject any payload length other than 33.

`setup_scene_parity_material.py` creates one texture-specific material instance per imported source material and consumes the complete catalog material object verbatim: `slot`, `sourceMaterialIndex`, `schemaVersion`, `mode`, `opacity`, `rawTranspType`, `effectiveTranspType`, `alphaTestEnabled`, `alphaRef`, `alphaFunc`, `alphaBlendEnabled`, `srcBlend`, and `destBlend`. Because blend mode and payload expression are compiled parent properties, it creates two explicit five-parent families from the same lighting equation:

```text
static/HISM (PerInstanceCustomData):
  M_SceneParity_Static_Opaque
  M_SceneParity_Static_Masked
  M_SceneParity_Static_Alpha
  M_SceneParity_Static_Additive
  M_SceneParity_Static_Subtractive
skeletal (CustomPrimitiveData):
  M_SceneParity_Skeletal_Opaque
  M_SceneParity_Skeletal_Masked
  M_SceneParity_Skeletal_Alpha
  M_SceneParity_Skeletal_Additive
  M_SceneParity_Skeletal_Subtractive
```

Opaque uses `BLEND_Opaque`; masked uses `BLEND_Masked` and the catalog-derived cutoff; alpha uses `BLEND_Translucent`; additive uses `BLEND_Additive`. Subtractive is a dedicated `BLEND_Modulate` parent whose Emissive/modulate factor is `1 - saturate(legacyLitRgb)`, so hardware composition is literally `dst * (1 - src)`; it is not routed through alpha/additive opacity wiring. Source `opacity` remains in metadata, but does not scale subtractive RGB because the original opacity stage changes alpha while `ZERO/INVSRCCOLOR` consumes source RGB. A material instance always chooses the parent matching both `assetKind` and legacy mode, retains `BaseColorTexture` and every source metadata field above, and never guesses from texture alpha. Literal tests cover byte-for-byte metadata propagation, all ten `(assetKind,mode)` parent choices, alpha cutoff `130/255` for source `ALPHAREF=129`, additive output/opacity wiring, subtractive `2 -> 5` normalization, `ZERO/INVSRCCOLOR`, and its inverse-source-color modulate wiring. Unknown modes, raw/effective mismatches, and unsupported source blend metadata are fatal before any asset is saved. The shared lighting equation is:

```text
legacy-unlit:
    output = texture
lit:
    directional = envEnabled
        * max(dot(WorldNormal, -ueLightDirection), 0)
        * directionalColor
    pointN = pointEnabled
        * inRangeN
        * max(dot(WorldNormal, LN), 0)
        * pointNRGB
        / max(att0N + att1N*dN + att2N*dN*dN, epsilon)
    output = texture * saturate(resolvedAmbient + directional + point1 + point2)
all modes:
    outputAlpha = textureAlpha * sourceOpacity
masked only:
    OpacityMask = outputAlpha, clip = (alphaRef + 1) / 255
alpha/additive:
    Opacity = outputAlpha
subtractive:
    src = saturate(output.rgb)
    Emissive/modulate factor = 1 - src
    framebuffer result = dst * (1 - src)
```

`resolvedAmbient` already includes the legacy shade/tile-color rule and must not be multiplied a second time. Static parents declare only StaticMesh plus InstancedStaticMeshes; opaque/masked static parents may declare Nanite, while alpha/additive/subtractive static components set `bDisallowNanite`. Skeletal parents declare SkeletalMesh and explicitly do not rely on InstancedStaticMeshes or Nanite usage. The checker rejects a parent from the other transport family, mismatched raw/effective types, mode/blend/cutoff/wiring, missing required usage, unexpected cross-kind usage, fallback/default material, or alpha/additive/subtractive Nanite. Runtime export reports subtractive separately from alpha/additive and includes the effective parent, `BLEND_Modulate`, inverse-source-color wiring fingerprint, and Nanite prohibition; a generic "translucent" bucket cannot close the gate.

- [ ] **Step 4: Verify RED**

```bash
./tools/AssetConverter/build/AssetConverter scene-map-fixture \
  --contract tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json \
  --output <run>/synthetic/nonidentity-skinned
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_parity_placement \
  CorsairsUE.Scripts.tests.test_scene_parity_test_fixture -v
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex -MaxParallelActions=2
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -run=pythonscript \
  -script="CorsairsUE/Scripts/import_scene_parity_test_fixture.py --fixture-manifest $PWD/<run>/synthetic/nonidentity-skinned/synthetic-nonidentity-skinned.json --content-root /Game/SceneParityTests/<runId> --report $PWD/<run>/synthetic/nonidentity-skinned/headless-import.json"
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -CorsairsSceneParitySyntheticFixtureReport="$PWD/<run>/synthetic/nonidentity-skinned/headless-import.json" \
  -ExecCmds="Automation RunTests Corsairs.Import.SceneParity" \
  -TestExit="Automation Test Queue Empty"
```

Expected RED: missing complete catalog animation/material fields, importer/explicit manifest validation, current schema/yaw/height or payload transport, and synthetic single-vs-double-bake assertions fail. A missing manifest argument, fallback directory scan, zero tests, or environment skip is not RED evidence.

- [ ] **Step 5: Implement headless placement**

Load the existing `/Game/Maps/Garner` level; never create a blank transient replacement. This placement helper removes/rebuilds only actors already tagged `CorsairsSceneParity`; Task 8 separately owns classification and removal of pre-tag legacy output. Load real meshes/materials only from `/Game/SceneParity/<runId>`. Create deterministic HISM groups for static parts, assign 33 per-instance custom floats, and spawn deterministic tagged `ASkeletalMeshActor` instances whose components receive exactly 33 CPD floats and the catalog's complete paused tick-120 animation object/reference pose. Verify the imported part-root metadata before placement; both paths use only the anchor transform, positive actor scale, stable labels/component names, and the parent family dictated by `(assetKind,mode)`. In the automation-only path, `import_scene_parity_test_fixture.py` requires `--fixture-manifest`, validates its kind/contract/glTF/bin hashes, imports into the explicit test root, and runs the single-vs-double-bake assertions. It atomically writes schema-v1 `headless-import.json` with `status=PASS`, exact content root, fixture-manifest path/SHA, tracked contract path/SHA, glTF/bin paths/SHA, imported asset paths, literal part-root/bounds/root-bone evidence, and `issues=[]`; missing or failed evidence publishes no PASS report. It destroys the test world without adding it to the real placement manifest. Do not save the level within this script; Task 8 owns final validation and save.

- [ ] **Step 6: Verify GREEN and commit**

```bash
./tools/AssetConverter/build/AssetConverter scene-map-fixture \
  --contract tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json \
  --output <run>/synthetic/nonidentity-skinned
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_parity_placement \
  CorsairsUE.Scripts.tests.test_scene_parity_test_fixture -v
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex -MaxParallelActions=2
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -run=pythonscript \
  -script="CorsairsUE/Scripts/import_scene_parity_test_fixture.py --fixture-manifest $PWD/<run>/synthetic/nonidentity-skinned/synthetic-nonidentity-skinned.json --content-root /Game/SceneParityTests/<runId> --report $PWD/<run>/synthetic/nonidentity-skinned/headless-import.json"
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -CorsairsSceneParitySyntheticFixtureReport="$PWD/<run>/synthetic/nonidentity-skinned/headless-import.json" \
  -ExecCmds="Automation RunTests Corsairs.Import.SceneParity" \
  -TestExit="Automation Test Queue Empty"
git add \
  CorsairsUE/Source/CorsairsImport/Public/SceneManifest.h \
  CorsairsUE/Source/CorsairsImport/Private/SceneManifest.cpp \
  CorsairsUE/Source/CorsairsImport/Private/Tests/SceneManifestTests.cpp \
  CorsairsUE/Scripts/scene_parity_placement.py \
  CorsairsUE/Scripts/place_scene_parity.py \
  CorsairsUE/Scripts/setup_scene_parity_material.py \
  CorsairsUE/Scripts/check_scene_parity_materials.py \
  CorsairsUE/Scripts/import_scene_parity_test_fixture.py \
  CorsairsUE/Scripts/tests/test_scene_parity_placement.py \
  CorsairsUE/Scripts/tests/test_scene_parity_test_fixture.py
git commit -m "feat(ue): place scene parity instances"
```

---

### Task 8: Gate and atomically rebuild the Garner reference scene

**Task brief:** Reproducibly bootstrap or attest the canonical Garner base from tracked terrain inputs, prove its staged scene is a clean replacement, atomically publish it, then capture both instrumented clients under the same tick/camera contract. Inputs are all prior scene manifests plus the terrain-plan Task 7 manifest/Task 8 reports; outputs are a nonblank terrain-and-GameMode `.umap`, a recoverable scene publication, whole-world runtime evidence, and distinct tick-120 PNGs; hidden Content, ambiguous legacy ownership, protected-actor mutation, stale builds, inexact ticks, or failed byte restoration are fatal.

**Files:**

- Create: `CorsairsUE/Scripts/scene_parity_check.py`
- Create: `CorsairsUE/Scripts/check_scene_parity.py`
- Create: `CorsairsUE/Scripts/export_scene_parity_runtime.py`
- Create: `CorsairsUE/Scripts/legacy_scene_cleanup.py`
- Create: `CorsairsUE/Scripts/configure_garner_scene_base.py`
- Create: `CorsairsUE/Scripts/rebuild_garner_scene.py`
- Create: `CorsairsUE/Scripts/atomic_publish_garner.py`
- Create: `CorsairsUE/Scripts/capture_scene_parity_material_modes.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_parity_check.py`
- Create: `CorsairsUE/Scripts/tests/test_legacy_scene_cleanup.py`
- Create: `CorsairsUE/Scripts/tests/test_rebuild_garner_scene.py`
- Create: `CorsairsUE/Scripts/tests/test_scene_parity_material_modes.py`
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsParityCaptureSubsystem.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsParityCaptureSubsystem.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsParityCaptureTests.cpp`
- Create: `CorsairsUE/Data/Capture/garner_reference_capture.json`
- Create: `CorsairsUE/Data/Capture/scene_subtractive_capture.json`
- Create: `scripts/build_garner_reference.py`
- Create: `scripts/build-original-parity-client.ps1`
- Create: `scripts/capture_garner_parity.py`
- Create: `scripts/tests/test_capture_garner_parity.py`
- Create: `sources/Client/src/App/OriginalParityCapture.h`
- Create: `sources/Client/src/App/OriginalParityCapture.cpp`
- Modify: `sources/Client/src/App/Main.cpp`
- Modify: `sources/Client/src/App/GameAppInterface.cpp`
- Modify: `sources/Client/Game.vcxproj`
- Modify: `scripts/dev/run-original-client.sh`
- Modify: `tools/AssetConverter/src/Main.cpp`
- Modify: `CorsairsUE/Scripts/place_objects.py`
- Modify: `CorsairsUE/Scripts/place_all_maps.py`
- Modify: `CorsairsUE/Scripts/check_placement.py`

**Checker interfaces:**

- `validate_source_manifest(manifest: dict) -> list[str]`
- `validate_garner_base_prerequisite(bundle: dict, actual: dict) -> list[str]`
- `validate_runtime_scene(source: dict, planned: dict, actual: dict) -> list[str]`
- `build_legacy_expectations(source: dict, model_map: dict) -> dict`
- `classify_legacy_actor(actor: dict, expectations: dict) -> tuple[str, str]`
- `build_capture_camera_matrix(contract: dict) -> tuple[tuple[float, ...], ...]`
- `projected_bounds_intersect_viewport(bounds: dict, camera_contract: dict) -> bool`

**Capture/build pure interfaces:**

- `validate_original_build_command_plan(steps: list[dict]) -> list[str]`
- `validate_original_build_attestation(data: dict, repo_root: Path) -> list[str]`

Both functions live in `scripts/capture_garner_parity.py`, import no Windows/CrossOver modules, and are exercised with temporary files on macOS/Linux. The command-plan validator requires an explicit FreeType project rebuild as step 0 and the solution `Game` target as step 1. The attestation validator derives the current tracked FreeType extension set, validates every attested evaluated-item kind/path, and reads and hashes every declared repo input/output itself; it never accepts a report's hash without file I/O. It always compares against an existing canonical pre-build snapshot, so every input-record or current-byte mismatch it reports is `STALE_FREETYPE_INPUT`; `MISSING_FREETYPE_INPUT` belongs only to initial discovery before that snapshot exists.

- [ ] **Step 1: Add RED checker tests**

Require violations for:

- missing source key;
- duplicate `(sourceKey,partIndex)`;
- catalog part-count mismatch;
- XY or Z error 1.01 cm;
- forward dot 0.9989;
- negative scale;
- any type-1 mesh instance;
- a skeletal payload present only as dynamic-material parameters instead of component `CustomPrimitiveData`;
- a static payload not present as exactly 33 HISM per-instance values;
- any static/skeletal parent-family swap, catalog/runtime material-mode mismatch, masked-cutoff mismatch, or additive material reported as ordinary alpha;
- any lost Task 3 material field (`slot`, `sourceMaterialIndex`, `schemaVersion`, `mode`, `opacity`, raw/effective transparency, alpha-test fields, alpha-blend flag, or source/destination blend), failure to preserve raw `2`/effective `5`, subtractive material reported as alpha/additive/generic translucent, non-`BLEND_Modulate` subtractive parent, missing inverse-source-color wiring fingerprint, or subtractive Nanite;
- identity or double-applied `partRootMatrix` on the literal skinned non-identity fixture;
- absent/island0 object whose projected bounds intersect the viewport.

Require 1.00 cm, dot 0.999, and positive scale to pass.

The checker input called `actual` is not the placement plan. `export_scene_parity_runtime.py` loads the staging/canonical level and enumerates **every actor in the loaded world**, including unloaded/hidden level actors where the non-World-Partition package API exposes them; it does not begin from the `CorsairsSceneParity` tag. For tagged parity actors it records static HISM mesh/material path, instance transform, exactly 33 per-instance floats, bounds and component/instance index; and skeletal mesh, animation asset/mode/loop/playback time/paused state, material path, raw component `CustomPrimitiveData[0..32]`, bounds, actor label, imported part-root matrix/root-bone evidence, and actor transform. It records effective parent path, every source material field (`slot`, `sourceMaterialIndex`, `schemaVersion`, `mode`, `opacity`, `rawTranspType`, `effectiveTranspType`, `alphaTestEnabled`, `alphaRef`, `alphaFunc`, `alphaBlendEnabled`, `srcBlend`, `destBlend`), effective blend mode/cutoff, subtractive wiring fingerprint, all material usage flags, Nanite state, loaded map package, terrain actor count, GameMode class, protected actor GUIDs, removed legacy GUIDs, and every untagged `/Game/All` scene candidate. Tests prove that changing only an actual static or skeletal transform, either payload value/transport, mesh/animation path or playback state, part-root evidence, any material metadata field, parent/wiring/usage flag, asset-kind mapping, or protected GUID fails even when the planned manifest remains correct.

Add pure legacy-classifier fixtures based on the exact old tracked writers and schema-v1 math. `build_legacy_expectations` uses all source records (including the old type-1 bug), `CorsairsUE/Scripts/model_map.json`, old location `(x,-y,heightOff)`, and old yaw `yaw/10` to form asset/transform multisets. Classification is intentionally narrow:

```text
generated-group:
  untagged exact Actor, label Inst_<asset basename> (UE numeric suffix allowed),
  exactly one HISM plus its default root, /Game/All mesh,
  instance-transform multiset exactly equals the legacy expectation
generated-fallback:
  untagged exact StaticMeshActor or SkeletalMeshActor, /Game/All mesh,
  default component layout, and one uniquely attributable legacy transform
unrelated:
  actor has no generated label/default-layout signature; preserve it even
  when it uses /Game/All (for example a gameplay actor with extra components)
ambiguous:
  generated label/layout with partial/extra transforms, duplicate claim,
  an Inst_* candidate with extra gameplay component/tag/attachment, or a
  default fallback transform with zero/multiple possible legacy owners
```

Run classification for the entire staging world before deleting anything. Unit fixtures include: exact `Inst_*` group -> generated; partial group -> ambiguous; `/Game/All` gameplay actor with an extra gameplay component -> unrelated/preserved; exact skeletal fallback -> generated; two identical possible owners -> ambiguous. After rebuild, `export_scene_parity_runtime.py` reruns the classifier; any `generated-*` or `ambiguous` leftover is a checker failure even though it lacks the parity tag.

`build_capture_camera_matrix` is independent from `GetObjectLocation`/`GetObjectRotation`. Its fixture uses the literal approved contract and golden source anchors, not production actor helpers:

```text
viewport = 1920x1080, aspect 16:9
target = (223325,-278475,100) cm
arm = 6103.2778 cm
pitch = -55.00798 deg
yaw = 90 deg
horizontal FOV = 54.0222067 deg
spring collision = off
landmarks =
  ID22  (223200,277320)
  ID314 (222610,278550)
  ID314 (223880,278550)
  ID323 (222550,278180)
  ID323 (223920,278170)
```

For each landmark the source-manifest Z and the independent camera matrix produce the expected pixel. Projecting the corresponding actual HISM origin/bounds must differ by no more than 5 pixels. Altering one actual landmark by the world-space amount equivalent to 5.01 pixels is RED; 5.00 pixels is GREEN.

Also add the bootstrap/capture tests with these literal names:

```text
test_clean_checkout_requires_tracked_terrain_bootstrap
test_base_gate_requires_manifest_report_hash_chain
test_base_gate_requires_reference_terrain_and_corsairs_game_mode
test_runtime_export_rejects_untagged_legacy_leftover_in_whole_world
test_original_build_uses_solution_game_target_and_dependencies
test_original_build_attestation_rejects_stale_input_or_output
test_command_plan_requires_freetype_before_solution_game
test_command_plan_rejects_freetype_after_solution_game
test_attestation_requires_all_freetype_input_hashes
test_attestation_rejects_missing_or_changed_freetype_resource_input
test_attestation_rejects_stale_freetype_input_or_toolchain
test_attestation_rejects_missing_or_stale_freetype_output
test_original_tick_120_requires_logical_delta_evidence
test_system_ini_restore_is_byte_exact_on_every_exit
```

In `OriginalBuildContractTests`, construct one literal valid two-step command plan, one canonical pre-build FreeType input snapshot, and one valid sidecar over real project/source/output files in a temporary Git worktree. Its FreeType input fixture must include the exact normalized repo-relative path `sources/Libraries/FreeType/src/base/ftver.rc`, kinds `ResourceCompile` and `tracked-extension`, and the SHA-256 of real tracked temporary `.rc` bytes. The sole pre-baseline discovery fixture makes that required input nonexistent before any snapshot is written and expects `MISSING_FREETYPE_INPUT`. After the valid snapshot exists, table mutations must delete the FreeType step, move it after `Game`, replace its project path, remove any input record, add one, remove or rename the `ftver.rc` path, change its `ResourceCompile` kind, omit or alter any recorded hash, alter the `.rc` or another input's bytes, omit the output record, tamper `freetype.lib`, or change the attested MSBuild/compiler identity. Every post-baseline input mutation—missing/added/removed/renamed path, changed kind, missing/changed recorded hash, or changed current bytes/hash—must produce exactly `STALE_FREETYPE_INPUT`, never `MISSING_FREETYPE_INPUT`. Require the exact issue codes `MISSING_FREETYPE_STEP`, `FREETYPE_ORDER`, `FREETYPE_PROJECT`, `MISSING_FREETYPE_INPUT`, `STALE_FREETYPE_INPUT`, `MISSING_FREETYPE_OUTPUT`, `STALE_FREETYPE_OUTPUT`, and `FREETYPE_TOOLCHAIN`; a generic nonempty-error assertion is insufficient. `test_attestation_requires_all_freetype_input_hashes` expects `STALE_FREETYPE_INPUT` for a post-baseline missing record/hash. `test_attestation_rejects_missing_or_changed_freetype_resource_input` runs the literal post-baseline `.rc` record/path/kind/hash/byte mutations as named subtests and expects exactly `STALE_FREETYPE_INPUT` for every one.

- [ ] **Step 2: Run the focused Task 8 RED before implementation**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_parity_check \
  CorsairsUE.Scripts.tests.test_legacy_scene_cleanup \
  CorsairsUE.Scripts.tests.test_rebuild_garner_scene \
  CorsairsUE.Scripts.tests.test_scene_parity_material_modes \
  scripts.tests.test_capture_garner_parity -v

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_command_plan_requires_freetype_before_solution_game \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_command_plan_rejects_freetype_after_solution_game \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_requires_all_freetype_input_hashes \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_rejects_missing_or_changed_freetype_resource_input \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_rejects_stale_freetype_input_or_toolchain \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_rejects_missing_or_stale_freetype_output -v
```

Expected on the first RED run: the five test modules exist, but collection fails with `ModuleNotFoundError` for the still-missing production modules `CorsairsUE.Scripts.scene_parity_check`, `CorsairsUE.Scripts.legacy_scene_cleanup`, `CorsairsUE.Scripts.rebuild_garner_scene`, `CorsairsUE.Scripts.capture_scene_parity_material_modes`, and `scripts.capture_garner_parity`. After adding import-only stubs, the named tests above must still fail because there is no terrain bootstrap/report-chain gate, no whole-world leftover rejection, no subtractive parent/readback gate, no two-step FreeType-before-Game command validator, no FreeType input/output/toolchain attestation (including `ResourceCompile`/`ftver.rc`), no tick-120 logical-delta proof, and no exact-byte `system.ini` rollback. In the six-test focused command the initial stub failures must specifically expose the missing validation; once issue DTOs exist, the `.rc` mutations and the other mutations must produce the literal codes above. A zero-test run, an import skip, an environment-dependent skip, or a pass that merely finds a pre-existing `freetype.lib` is not an acceptable RED.

- [ ] **Step 3: Implement actual-runtime integration gates**

The real Garner run must require:

```text
ReferenceObjectSet = 1634
island1 = 1625
island2 = 6
absent/island0 = 3
type1 mesh instances = 0
143 formerly false instances inside 61.03 m = 0
all source-key part counts equal catalog
XY error <= 1 cm
Z error <= 1 cm
forward dot >= 0.999
five landmark screen errors <= 5 px
all three absent-object bounds outside viewport
actual HISM and skeletal mesh/material/payload counts equal plan
skeletal payload transport = CustomPrimitiveData[0..32]
static payload transport = 33 HISM per-instance floats
part-root matrix/root-bone evidence equals catalog for every skinned part
skeletal animation/reference pose and paused tick-120 time equal catalog
legacy generated/ambiguous leftovers across whole world = 0
all classified legacy generated GUIDs removed exactly once
material fallback/usage/translucent-Nanite errors = 0
material parent kind/mode/cutoff mismatches = 0
material raw/effective transparency mismatches = 0
subtractive parent/wiring/Nanite errors = 0
full-corpus material census has exactly 48 used raw2 -> effective5
  subtractive materials across exactly 16 LMO
all 79 real skinned parts checked exactly once;
  real nonidentity-skinned census = []
tracked synthetic nonidentity-skinned headless gate = PASS,
  with zero synthetic assets/records in real manifests and placement
dedicated subtractive linear capture for both static and skeletal parents = PASS
base bundle has the complete valid terrain-manifest/report SHA chain
loaded map has exactly the checked reference terrain actor and >= 1 terrain actor
loaded map GameMode = /Script/CorsairsGame.CorsairsGameMode
original build step 0 = explicit FreeType Rebuild; step 1 = solution Game
freetype.lib + every FreeType project/source/ResourceCompile/toolchain SHA attestation is current
all protected gameplay/unrelated actor GUIDs unchanged
second build has identical sourceKey/instanceIndex/counts
```

- [ ] **Step 4: Make legacy scripts delegate or refuse schema v2**

`place_objects.py`, `place_all_maps.py`, and `check_placement.py` either call shared pure scene-parity helpers or exit nonzero when handed a schema-v2 manifest. They may not keep a second transform/type implementation.

- [ ] **Step 5: Implement a staged Unreal rebuild**

Implement `configure_garner_scene_base.py` as the small tracked bridge between the terrain-plan seed import and its two checked idempotence passes. It accepts exactly `LEVEL_BUILD_REPORT SEED_IMPORT_REPORT MAP_PACKAGE GAME_MODE_CLASS REPORT`, validates both input reports and their manifest/hash identity, requires the current five package hashes to equal the seed import's `finalPackageHashes`, and independently requires `/Game/Maps/Garner`, `/Game/Maps/Garner.Garner`, the single `ReferenceTerrainBuildRoot`, and the exact `ReferenceTerrain_Garner_17_21` actor/asset/transform. It loads `/Script/CorsairsGame.CorsairsGameMode`, sets only `WorldSettings.default_game_mode`, saves/reloads, and verifies the effective class. It atomically writes a schema-v1 `garner-scene-base-configuration` report containing the terrain-manifest, level-build, and seed-import paths/SHA-256, package/world/marker/reference-actor paths, literal GameMode class, before/after map hashes, and `issues`. Missing class/report/terrain/marker, a stale pre-map hash, a wrong package, save/reload failure, or nonempty issues is fatal; the legacy `setup_gameplay.py` is not used because it also creates unrelated gameplay state and has no machine-readable success contract.

`rebuild_garner_scene.py` is the only tracked Unreal Python scene-rebuild entry point. It:

1. validates `--base-bundle` before mutation. For a clean-checkout bootstrap the bundle must hash-link the Task 7 `garner.reference-albedo.json`, terrain Task 8 level-build report, seed-import report, GameMode configuration report, checked pass-1/pass-2 import reports, and final terrain checker report; every report has `issues=[]`, all manifest hashes agree, both checked passes are zero-mutation with identical final hashes, the configuration `afterMapSha256` equals their `/Game/Maps/Garner` final hash, and the checker actual hashes equal them. For an already scene-published canonical map, its prior scene run-manifest/canonical hash must extend the same base bundle; an arbitrary ignored `.umap` with no valid chain is not accepted as a prerequisite;
2. validates the explicit `--synthetic-fixture-report` before loading or mutating the map. The run-local report must be schema-valid and PASS, name the exact `/Game/SceneParityTests/<runId>` root, hash-link `<run>/synthetic/nonidentity-skinned/synthetic-nonidentity-skinned.json`, and transitively match that producer manifest's tracked contract plus glTF/bin paths and SHA-256 values. A missing argument, stale hash, directory discovery fallback, non-PASS single-vs-double-bake evidence, or any synthetic path in the real scene assets/catalog/placement aborts before map load;
3. loads `/Game/Maps/Garner` and rejects a blank/transient world and World Partition/external actors for this atomic path;
4. inspects the actual world independently of reports and requires the exact package/world, one `ReferenceTerrainBuildRoot`, one `ReferenceTerrain_Garner_17_21` using the canonical checked mesh/material and transform, at least one terrain actor, and `WorldSettings.default_game_mode == /Script/CorsairsGame.CorsairsGameMode`. On the fresh bootstrap the actual canonical map hash must equal the terrain checker map hash; on a later scene rebuild it must equal the prior scene run-manifest hash while the four immutable terrain asset hashes and actual terrain/GameMode signature still equal the base checker. Any mismatch aborts before staging duplication;
5. duplicates the canonical map into unique `/Game/Maps/__SceneParityStaging/Garner_<runId>`;
6. verifies the duplicate has identical terrain, GameMode, complete actor GUID set, and base-provenance signature;
7. enumerates the entire staging world, builds the exact legacy expectation multiset, classifies every actor, and writes a pre-mutation inventory with `generated`, `protected`, and `ambiguous` GUIDs; an expected group absent from the current map is harmless, but any present partial candidate multiset, ambiguity, duplicate claim, or classifier/export disagreement aborts before actor destruction;
8. removes exactly the preclassified legacy-generated actors plus actors tagged `CorsairsSceneParity`, then proves every protected gameplay/unrelated GUID, class, attachment, component signature, transform, and package still matches the duplicated baseline;
9. rebuilds the new `CorsairsSceneParity` population and runs a second whole-world classification; any untagged generated/ambiguous `/Game/All` leftover is fatal;
10. calls `export_scene_parity_runtime.py`, material checks, and `validate_runtime_scene`;
11. saves only the staging package and writes a success report containing its package path, `.umap` path, SHA-256, source-map SHA, base-bundle SHA, synthetic fixture report path/SHA and validated producer-manifest/asset hash chain, pre/post whole-world inventories, removed legacy GUIDs, protected-actor digest, and actual-runtime manifest.

It never saves `/Game/Maps/Garner`. `atomic_publish_garner.py` runs only after the graphical editor has closed, validates the report and both package hashes, preserves a unique backup, and uses same-volume `os.replace` to publish the single non-World-Partition `.umap`. On any failure it restores the backup. It then reopens canonical Garner in a fresh headless run, exports all world actors again, reruns the legacy-leftover classifier and every gate, and deletes neither backup nor prior immutable `/Game/SceneParity/<runId>` assets. Failure before final reopen leaves the prior canonical map recoverable.

The Unreal commands are tracked and literal:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -run=pythonscript \
  -script="CorsairsUE/Scripts/rebuild_garner_scene.py --map /Game/Maps/Garner --base-bundle <run>/garner-base-bundle.json --run-manifest <run>/placement.json --synthetic-fixture-report <run>/synthetic/nonidentity-skinned/headless-import.json --report <run>/staging-report.json"
```

Before publication, run a separate graphical material capture; the fixed Garner screenshot is not a substitute for this mode-level gate. `scene_subtractive_capture.json` defines a linear RGBA16F offscreen target with tone mapping/exposure disabled, destination RGB `(0.8,0.5,0.25)`, and legacy-unlit source RGB `(0.25,0.4,0.8)`. `capture_scene_parity_material_modes.py` renders the same overlap once through the static subtractive parent and once through the skeletal subtractive parent and reads back the pre-tonemap pixels. Both must equal literal `(0.6,0.3,0.05)` within `1/1024`, i.e. `dst * (1 - src)`, and must differ from literal alpha/additive control renders. Its atomic report records parent paths, `rawTranspType=2`, `effectiveTranspType=5`, `BLEND_Modulate`, inverse-source-color wiring fingerprints, Nanite-disabled facts, linear input/output values, image hashes, and issues. Missing RHI/readback, default-material fallback, a generic translucent route, sRGB/tonemap contamination, or reuse of one parent for both asset kinds is fatal.

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_parity_material_modes -v
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NoSound -RenderOffscreen \
  -run=pythonscript \
  -script="CorsairsUE/Scripts/capture_scene_parity_material_modes.py --contract CorsairsUE/Data/Capture/scene_subtractive_capture.json --report <run>/subtractive-material-capture.json"
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/atomic_publish_garner.py \
  --project "$PWD/CorsairsUE/CorsairsUE.uproject" \
  --report <run>/staging-report.json \
  --material-capture-report <run>/subtractive-material-capture.json
```

This graphical process runs alone under the global thermal/process rules and must exit before staging publication or either client capture. The report and its images remain run-local generated evidence and are hash-linked by `run-manifest.json`.

- [ ] **Step 6: Implement the tracked build orchestrator**

`scripts/build_garner_reference.py` runs, in order:

```bash
cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tools/AssetConverter/build -j2
ctest --test-dir tools/AssetConverter/build -j1 --output-on-failure

# Clean-checkout prerequisite from Garner terrain plan Tasks 7-8.
./tools/AssetConverter/build/AssetConverter terrain-reference \
  --map Client/map/garner.map \
  --database databases/gamedata.sqlite \
  --client-root Client \
  --alpha Client/texture/terrain/alpha/total.png \
  --output artifacts/maps \
  --page 17 21 \
  --require-present-rect 2193 2756 80 47 \
  --max-rss-mib 128 \
  --max-cache-mib 32 \
  --max-png-mib 96 \
  --max-height-error-cm 5 \
  --max-rms-error-cm 2

"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex -MaxParallelActions=2
UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/build_reference_terrain_level.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner <run>/bootstrap/reference-terrain-level-build.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner <run>/bootstrap/reference-terrain-import-seed.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/configure_garner_scene_base.py <run>/bootstrap/reference-terrain-level-build.json <run>/bootstrap/reference-terrain-import-seed.json /Game/Maps/Garner /Script/CorsairsGame.CorsairsGameMode <run>/bootstrap/garner-scene-base-configuration.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner <run>/bootstrap/reference-terrain-import-pass1.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner <run>/bootstrap/reference-terrain-import-pass2.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/check_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner <run>/bootstrap/reference-terrain-level-build.json <run>/bootstrap/reference-terrain-import-pass1.json <run>/bootstrap/reference-terrain-import-pass2.json <run>/bootstrap/reference-terrain-check.json" \
  -unattended -nop4 -NullRHI -NoSound

./tools/AssetConverter/build/AssetConverter scene-manifest \
  Client/map/garner.map Client/map/garner.obj <run>/garner
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/build_scene_reference_model_set.py \
  --source-manifest <run>/garner.objects.json \
  --database databases/gamedata.sqlite \
  --model-root Client/model/scene \
  --output <run>/reference-models.json
./tools/AssetConverter/build/AssetConverter \
  Client/model/scene <run>/models \
  --textures Client/texture/scene --profile scene-map \
  --required-models <run>/reference-models.json
./tools/AssetConverter/build/AssetConverter scene-map-fixture \
  --contract tools/AssetConverter/tests/fixtures/scene-map/nonidentity-skinned.json \
  --output <run>/synthetic/nonidentity-skinned
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/build_scene_catalog.py \
  databases/gamedata.sqlite <run>/models \
  <run>/garner.objects.json \
  <run>/reference-models.json \
  <run>/garner.scene-catalog.json \
  --content-root /Game/SceneParity/<runId>
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/build_scene_placement_manifest.py \
  <run>/garner.objects.json \
  <run>/garner.scene-catalog.json \
  <run>/placement.json
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s CorsairsUE/Scripts/tests -v
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_scene_parity_test_fixture.py --fixture-manifest $PWD/<run>/synthetic/nonidentity-skinned/synthetic-nonidentity-skinned.json --content-root /Game/SceneParityTests/<runId> --report $PWD/<run>/synthetic/nonidentity-skinned/headless-import.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -CorsairsSceneParitySyntheticFixtureReport="$PWD/<run>/synthetic/nonidentity-skinned/headless-import.json" \
  -ExecCmds="Automation RunTests Corsairs.Import.SceneParity" \
  -TestExit="Automation Test Queue Empty"
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/rebuild_garner_scene.py --map /Game/Maps/Garner --base-bundle $PWD/<run>/garner-base-bundle.json --run-manifest $PWD/<run>/placement.json --synthetic-fixture-report $PWD/<run>/synthetic/nonidentity-skinned/headless-import.json --report $PWD/<run>/staging-report.json"
```

The literal clean-checkout entry point is:

```bash
python3 scripts/build_garner_reference.py --bootstrap-if-missing
```

`--bootstrap-if-missing` runs the terrain block above only when the canonical `.umap` is absent. The first import is the seed that creates the terrain actor/assets; after the GameMode-only configuration, pass 1 and pass 2 must both report zero mutations and identical five-package hashes, so the unmodified terrain Task 8 checker can validate them and the configuration hash can be linked exactly to their map hash. The flag is not allowed to replace an existing map: an existing map must pass the prior base-bundle plus actual terrain/GameMode gates, otherwise the command stops and prints the exact opt-in recovery command `python3 scripts/build_garner_reference.py --replace-base-from-tracked-inputs` without invoking it. Without either a valid existing base or this opt-in clean bootstrap, the orchestrator exits nonzero before scene conversion or staging. The terrain command and three terrain scripts are the tracked outputs of `docs/superpowers/plans/2026-08-07-garner-terrain-parity.md` Tasks 7-8; missing commands/scripts, a missing `artifacts/maps/garner.reference-albedo.json`, any nonzero process exit, absent report, stale SHA, nonempty report `issues`, a mutating checked pass, wrong world/marker/reference actor, zero terrain actors, or the wrong GameMode is a hard prerequisite failure.

After the clean block, the orchestrator validates every report and writes `<run>/garner-base-bundle.json` atomically. The bundle contains normalized paths and SHA-256 for the terrain manifest, level-build, seed import, GameMode configuration, both checked imports, and terrain-check reports; expected package/world/marker/reference-actor paths; `/Script/CorsairsGame.CorsairsGameMode`; immutable terrain asset hashes; and the pre-scene canonical map hash. Only then may it call `rebuild_garner_scene.py`. This makes the bootstrap's manifest/checker part of the staging authorization rather than a setup log.

The orchestrator uses blocking, sequential subprocess calls: it does not launch the next build/editor command until the prior process tree has exited, and after every `UnrealEditor-Cmd` invocation it verifies the owned editor PID tree is gone. Its failure/interrupt handler closes and reaps only the active owned tree before returning nonzero. The orchestrator uses `artifacts/scene-parity/runs/<sourceSha>.<uniqueId>/`; every scene intermediate and every bootstrap report is run-relative, while the terrain plan's atomically published top manifest remains at its specified `artifacts/maps/garner.reference-albedo.json` path and is copied by hash into the base bundle. It publishes `run-manifest.json` last with normalized paths and SHA-256 for the complete base bundle/report chain, source map/object files, Task 4-derived `reference-models.json`, real `scene-assets.json`, the tracked synthetic contract, `<run>/synthetic/nonidentity-skinned/synthetic-nonidentity-skinned.json`, its exact glTF/bin files, `<run>/synthetic/nonidentity-skinned/headless-import.json`, the five-mode/full-corpus census, catalog, every real `.gltf/.bin/texture`, placement manifest, Unreal runtime report, subtractive material-capture report/images, prior/canonical `.umap`, and protected/base signatures. It validates the contract -> producer manifest -> glTF/bin -> headless report hash chain and requires the headless single-vs-double-bake result to be PASS, while explicitly proving that no synthetic path occurs in real `reference-models.json`, `scene-assets.json`, catalog, placement, runtime population, or real census. It imports immutable real assets under `/Game/SceneParity/<runId>`, performs the staged rebuild/publish, and reruns the scene operation against the prior successful run-manifest to prove actual HISM mapping idempotence without rebuilding or erasing the canonical base. Any mismatch returns nonzero and cannot publish a new canonical map.

- [ ] **Step 7: Add a deterministic dual-client capture**

Implement two explicit protocol-v1 capture controllers; neither is active during a normal game launch.

`FCorsairsParityCaptureSubsystem` is a `UGameInstanceSubsystem` that subscribes to `FWorldDelegates::OnWorldPostActorTick`, enabled only by `-CorsairsParityCapture=<absolute-contract.json> -CorsairsParityCaptureResult=<absolute-result.json>`. It waits for the real `UCorsairsSession` to report InWorld and exact `(223325,278475)`, applies the camera/render contract, disables movement input, auto exposure, bloom, color grading, fog, clouds and motion blur, and requests UE fixed timestep `1/30`. Its counter increments only in the post-world-tick callback after a completed game tick with `DeltaSeconds == 1/30`; tick 120 arms the screenshot for that frame's viewport draw, and the screenshot callback must report the same engine frame number before result publication. The result records all 120 deltas, their SHA-256, world/session position, camera matrices, viewport, engine tick/frame IDs, screenshot SHA and exit code. A mismatched delta, server position, resolution, camera, map, screenshot frame, or tick exits nonzero.

`OriginalParityCapture` is a small client-side state machine wired into `Main.cpp` and `CGameApp::Run`, enabled only by `-CorsairsParityCapture=<absolute-contract.json> -CorsairsParityCaptureResult=<absolute-result.json> -CorsairsParityCredentials=<0600-temporary-file>`. The credentials file supplies fixture account/password/character/region/server to the existing auto-login flow without placing a password in the command line, tracked contract, result, or logs; capture mode rejects the legacy plaintext `autolog:` argument. Before arming, the ordinary loop may wait for login, character selection, map streaming and the real main character. Arming requires the logged-in account/character to equal the requested fixture, the real current scene/map, character `(223325,278475)`, zero movement/action, the 1920x1080 D3D backbuffer, and required scene resources; readiness timeout is wall-clock only and is not reported as simulation time.

Once armed, the original controller bypasses `SteadyFrameSync::Run()` and owns exactly 120 updates. For completed simulation tick `n=1..120` it passes this logical timestamp to `FrameMove`:

```cpp
LogicalTickMs(n) = baseTickMs + floor(n * 1000 / 30)
// deltas repeat 33,33,34 ms; tick 120 is exactly base + 4000 ms.
```

Each iteration performs `PollPackets`, one `FrameMove(LogicalTickMs(n))`, reapplies the literal camera and capture render states immediately after camera FrameMove, and one `Render`. It keeps the character stationary, forces terrain ambient white and D3D fog off, records that the DX9 path has no auto-exposure/cloud/motion-blur stages, and fails if the server or local anchor changes. On tick 120 it invokes the existing `MPRender::CaptureScreen(<explicit PNG>)` readback path immediately after that tick's render, writes result JSON by atomic rename, and exits cleanly. The result contains protocol version, build-manifest SHA, ticks completed, the literal timestamp list/SHA, `captureTick=120`, `captureHz=30`, exact map/position/angle, viewport/backbuffer, effective render-state facts, camera eye/target/view/projection matrices, PNG path/SHA, and evidence that the normal sleep-driven pacer was bypassed. Tests require the literal `33,33,34` delta cycle, `4000 ms` endpoint, and one capture callback at tick 120; no `sleep`, process uptime, file mtime, or keyboard PrintScreen event may establish the tick. The instrumentation still uses the original client's built-in screenshot implementation, not an OS screen grab.

The original camera adapter derives literal vertical FOV `32°` from horizontal `54.0222067°` and `16:9`, converts to the original client's metre units, and uses target `(2233.25,2784.75,1.0)` plus eye `(2233.25,2749.75,51.0)` derived independently from arm `61.032778 m`, pitch `-55.00798°`, and yaw `90°`. It calls `SetWorldViewFOV`/`LookAt`, writes back the effective D3D view/projection matrices, and fails if their independent projection of the five landmarks differs from the contract by more than the same 5 px gate. Literal unit tests assert those target/eye/FOV values; the adapter does not drive `CCameraCtrl` by synthetic mouse motion.

`garner_reference_capture.json` is tracked schema v1 and contains all literal camera values above, terrain ambient white, original HUD-mask rectangles, and the five landmark keys. It contains no credentials.

`scripts/build-original-parity-client.ps1` is the only accepted producer of the instrumented binary. On a Windows checkout with VS 2026 Preview and toolset v145 it resolves `MSBuild.exe` with `vswhere` and sets `$ErrorActionPreference = "Stop"`. It verifies from `sources/talesofpirates.sln` that the `Game` target declares `AudioSDL`, `Utils`, `Common`, `CorsairsNet`, and `MindPower3D`, while FreeType is only a sibling solution project and has **no** `Game` dependency edge. Because `Game.vcxproj` nevertheless links `freetype.lib`, `/t:Game` alone is not a clean-build contract. Before building, the script evaluates `freetype.vcxproj` with the same `Debug|x64`, `PlatformToolset=v145`, and explicit `SolutionDir` properties used by the build. It snapshots the project/import closure, the resolved `FullPath` of every evaluated repo-local `ClCompile`, `ClInclude`, and `ResourceCompile` item, and the union with every `git ls-files` tracked `.c`, `.h`, and `.rc` file under `sources/Libraries/FreeType`. A `ResourceCompile` path must resolve to a tracked repo-local `.rc`; the evaluated and tracked sets must both contain the literal `sources/Libraries/FreeType/src/base/ftver.rc`, which comes from `<ResourceCompile Include="..\..\..\src\base\ftver.rc" />`. This also covers platform files and headers included transitively rather than listed as project items. It creates separate binlog directories and executes these two commands in this exact order with no overlap:

```powershell
$solutionDir = (Resolve-Path "$repoRoot\sources").Path + `
  [IO.Path]::DirectorySeparatorChar
$freeTypeProject = Join-Path $solutionDir `
  "Libraries\FreeType\builds\windows\vc2010\freetype.vcxproj"
$freeTypeOutput = Join-Path $solutionDir "Libs\Debug\freetype.lib"

# Remove only this generated library so a successful step cannot reuse stale bytes.
if (Test-Path -LiteralPath $freeTypeOutput -PathType Leaf) {
  Remove-Item -LiteralPath $freeTypeOutput -Force
}
if (Test-Path -LiteralPath $freeTypeOutput) {
  throw "stale FreeType output could not be removed"
}

& $msbuild $freeTypeProject `
  /m:2 `
  /restore `
  /t:Rebuild `
  /p:Configuration=Debug `
  /p:Platform=x64 `
  /p:PlatformToolset=v145 `
  /p:CL_MPCount=2 `
  "/p:SolutionDir=$solutionDir" `
  "/bl:$repoRoot\artifacts\scene-parity\build\original-client-freetype-msbuild.binlog"
if ($LASTEXITCODE -ne 0) { throw "MSBuild FreeType rebuild failed" }
if (-not (Test-Path -LiteralPath $freeTypeOutput -PathType Leaf)) {
  throw "FreeType output is missing"
}
$freeTypeSha256BeforeGame = `
  (Get-FileHash -Algorithm SHA256 -LiteralPath $freeTypeOutput).Hash.ToLowerInvariant()

& $msbuild "$repoRoot\sources\talesofpirates.sln" `
  /m:2 `
  /restore `
  /t:Game `
  /p:Configuration=Debug `
  /p:Platform=x64 `
  /p:PlatformToolset=v145 `
  /p:CL_MPCount=2 `
  /p:BuildProjectReferences=true `
  "/bl:$repoRoot\artifacts\scene-parity\build\original-client-msbuild.binlog"
if ($LASTEXITCODE -ne 0) { throw "MSBuild Game solution target failed" }
$freeTypeSha256AfterGame = `
  (Get-FileHash -Algorithm SHA256 -LiteralPath $freeTypeOutput).Hash.ToLowerInvariant()
if ($freeTypeSha256AfterGame -ne $freeTypeSha256BeforeGame) {
  throw "FreeType output changed during Game build"
}
```

The input snapshot uses one record per canonical file. Resolve item paths relative to the project/import that defines them, reject paths outside the repository, convert them to exact-case repo-relative POSIX paths with `/`, merge duplicate paths while retaining a sorted `kinds` set (`project`, `import`, `ClCompile`, `ClInclude`, `ResourceCompile`, `tracked-extension`), and sort records by ordinal path bytes independent of host locale. Absolute paths, `.`/`..` segments, backslashes, case aliases, duplicate records, missing files, untracked resolved resources, and a missing `ftver.rc` record are fatal. During this initial discovery only, a required input that never existed prevents baseline creation and is `MISSING_FREETYPE_INPUT`. Store lowercase SHA-256 over exact bytes; neither timestamps nor MSBuild's incremental state count as evidence.

With the explicit `SolutionDir`, the FreeType project's `OutDir` and `TargetName` must evaluate to the literal `$(SolutionDir)Libs\Debug\freetype.lib`, i.e. `$repoRoot\sources\Libs\Debug\freetype.lib`. Immediately before `Game`, and again before publishing the sidecar, the script rebuilds the deterministic path/kind list, rereads and rehashes every FreeType input, and requires byte-for-byte equality with the canonical pre-build snapshot; it also requires the library hash above to remain unchanged. Once that baseline exists, any missing record, added/removed/renamed path, changed kind, missing/changed recorded hash, or changed bytes/current hash—including any post-build edit to `sources/Libraries/FreeType/src/base/ftver.rc`—is fatal with exactly `STALE_FREETYPE_INPUT`; it can never be reclassified as `MISSING_FREETYPE_INPUT`. Missing output, a changed/missing output, an unexpected solution edge/order, or either nonzero command is fatal. Removing the one exact generated `.lib` is allowed only inside this build script; it never deletes a directory or source file.

Directly invoking `sources/Client/Game.vcxproj` is forbidden because its other required libraries are solution dependencies. The explicit FreeType project rebuild is the required exception and must precede the solution command. The script fails if the two-step order, project path, solution target/dependency set, `Debug|x64` mappings, toolset, or expected output paths differ. It writes `Client/system/Game.parity-build.json` last and includes these attested inputs and outputs:

```text
inputs:
  sources/talesofpirates.sln path/SHA-256
  sources/Client/Game.vcxproj path/SHA-256
  sources/Libraries/AudioSDL/AudioSDL.vcxproj path/SHA-256
  sources/Libraries/Util/Utils.vcxproj path/SHA-256
  sources/Libraries/common/Common.vcxproj path/SHA-256
  sources/Libraries/CorsairsNet/CorsairsNet.vcxproj path/SHA-256
  sources/Engine/MindPower3D.vcxproj path/SHA-256
  sources/Libraries/FreeType/builds/windows/vc2010/freetype.vcxproj
    path/SHA-256
  every existing repo-local imported .props/.targets and every evaluated
    ClCompile/ClInclude/ResourceCompile input reachable from those seven projects,
    path/SHA-256
  sources/Client/src/App/OriginalParityCapture.h path/SHA-256
  sources/Client/src/App/OriginalParityCapture.cpp path/SHA-256
  sources/Client/src/App/Main.cpp path/SHA-256
  sources/Client/src/App/GameAppInterface.cpp path/SHA-256
  CorsairsUE/Data/Capture/garner_reference_capture.json path/SHA-256
  scripts/build-original-parity-client.ps1 path/SHA-256
  git revision + dirty-path list
toolchain:
  resolved MSBuild.exe path/version/SHA-256
  cl.exe path/version/SHA-256, PlatformToolset=v145, Windows SDK version
invocation:
  ordered buildSteps[0] = exact FreeType project path, target=Rebuild,
    configuration=Debug, platform=x64, toolset=v145, explicit SolutionDir,
    CL_MPCount=2, complete argument vector, exitCode=0,
    FreeType binlog path/SHA-256
  ordered buildSteps[1] = solution path, target=Game, configuration=Debug,
    platform=x64, toolset=v145, BuildProjectReferences=true,
    CL_MPCount=2, complete argument vector, exitCode=0,
    Game binlog path/SHA-256
freeType:
  projectPath/projectSha256
  sorted sourceInputs[] with normalized exact-case repo-relative POSIX path,
    sorted kinds[], and SHA-256 for every evaluated repo-local
    ClCompile/ClInclude/ResourceCompile/import input plus every tracked
    .c/.h/.rc file under sources/Libraries/FreeType
  required literal resource record:
    path=sources/Libraries/FreeType/src/base/ftver.rc,
    kinds include ResourceCompile and tracked-extension, SHA-256 over exact bytes
  outputExpression = $(SolutionDir)Libs\Debug\freetype.lib
  outputPath = sources/Libs/Debug/freetype.lib
  outputSha256 before Game and after Game, required identical
  MSBuild/cl.exe/toolset/SDK identity copied from the toolchain block
outputs:
  Client/system/Game.exe path/SHA-256/PE machine=x64
  Client/system/Game.pdb path/SHA-256
  sources/Libs/Debug/freetype.lib path/SHA-256
  sources/Libs/Debug/AudioSDL.lib path/SHA-256
  sources/Libs/Debug/Utils.lib path/SHA-256
  sources/Libs/Debug/Common.lib path/SHA-256
  sources/Libs/Debug/CorsairsNet.lib path/SHA-256
  sources/Libs/Debug/MindPower3D.lib path/SHA-256
  original-client-freetype-msbuild.binlog path/SHA-256
  original-client-msbuild.binlog path/SHA-256
```

The sidecar also records capture protocol version and is atomically renamed only after all required outputs exist and hash successfully. Before it can mutate the DB, `scripts/capture_garner_parity.py` independently enumerates the current tracked FreeType `.c`/`.h`/`.rc` files, validates the normalized/sorted/unique path-and-kind list, requires the literal evaluated `ResourceCompile` record for `sources/Libraries/FreeType/src/base/ftver.rc`, rereads every listed repo input/output, and recomputes every SHA-256. It also requires the exact two-step order and attested dependency list. Capture validation always has the canonical pre-build snapshot: any missing record, added/removed/renamed path, changed kind, missing/changed recorded hash, or changed bytes/current hash is exactly `STALE_FREETYPE_INPUT`, including a missing/renamed `ftver.rc` record or any post-build `.rc` edit; capture must never emit `MISSING_FREETYPE_INPUT`. Thus it rejects a missing/extra/stale FreeType project/source/resource/output/toolchain record before DB mutation, rather than trusting the Windows snapshot. It then runs the binary's opt-in `-CorsairsParityCaptureSelfTest=<result>` protocol through CrossOver. It waits for that self-test `Game.exe` to exit and immediately reaps its owned CrossOver/Wine process tree before any DB mutation. A missing Windows build environment does not silently reuse an arbitrary executable: the build script fails with reproducible prerequisite diagnostics; a Mac capture fails before DB mutation if the x64 binary/sidecar/FreeType/dependency outputs/self-test/process cleanup is absent, stale, or inconsistent. `run-original-client.sh` remains the sole launcher and only gains pass-through capture arguments after the required password argument.

`scripts/capture_garner_parity.py` accepts explicit loopback DB host, fixture account, and character and requires the password only through `CORSAIRS_FIXTURE_PASSWORD`. It creates the original client's credential file with `mkstemp` mode 0600 outside `artifacts`, deletes it in `finally`, and redacts it from subprocess diagnostics. Missing/empty credentials or any password occurrence in argv/result/log capture is fatal. Unit tests mock SQL/client processes and require this order:

```text
acquire exclusive capture lock; reject another Game.exe
recover and verify any prior interrupted system.ini journal
validate instrumented original build/self-test and UE build
create/redact 0600 fixture credential file from environment
snapshot exact Client/user/system.ini bytes/existence/mode/SHA to a 0600
same-directory recovery file; fsync file+directory
atomically patch only video.resolution=5, video.fullScreen=0,
gameOption.framerate=30, gameOption.vsync=0; verify parsed and raw patched SHA
validate loopback + validate exact fixture identity
read login_status and refuse unless it is already 0
snapshot character map/map_x/map_y/angle
set garner/223325/278475/90
run original sequentially -> consume protocol result tick 120/PNG
wait for Game.exe exit; immediately close and reap only the owned
CrossOver/Wine process tree; verify no owned original-client PID remains
run UE sequentially -> consume protocol result tick 120/PNG
wait for UnrealEditor exit; immediately reap its owned process tree;
verify no owned UE PID remains
on failure/timeout/interruption, finally terminates whichever single client
is active, verifies its process tree is gone, then restores the row
restore character row and verify it
atomically restore system.ini exact original bytes/mode
or remove it if originally absent; fsync and verify SHA/existence
delete the temporary credential file and verify absence
verify login_status was never mutated
remove recovery journal and release lock only after both restores verify
verify two different 1920x1080 PNG files
write build hashes, timestamp/delta hashes, viewport, HUD mask,
runtime states, landmark errors, and system.ini before/after SHA
compose side-by-side PNG
```

Restoration is exact-byte, not an INI round trip. Success, client failure, timeout, `KeyboardInterrupt`, and injected exception fixtures all require the original bytes and mode afterward. Before first mutation a durable journal names the recovery file and expected SHA; the next invocation restores it before doing anything else after an uncatchable process death. Backup contents never enter `artifacts` or logs and are deleted only after verification. Failure to snapshot, lock, fsync, restore, or verify is fatal and leaves the recovery file/journal in place with a printed recovery command.

The runner launches the original only through `scripts/dev/run-original-client.sh` and never starts a second `Game.exe`. UE runs at `-ResX=1920 -ResY=1080 -Windowed -UseFixedTimeStep -FPS=30 -CorsairsParityCapture=... -CorsairsParityCaptureResult=...`. Both clients run sequentially against the same offline fixture; after each result the runner waits for normal exit, immediately terminates/reaps only its still-owned child tree on a bounded timeout, and proves `Game.exe` plus owned CrossOver/Wine helpers or the owned `UnrealEditor` tree is gone before starting the next client. The same cleanup runs in `finally`; no capture process remains idle while the other client runs. The runner waits for protocol result files/process exits and never sleeps to infer tick completion. Live/non-loopback DB, nonzero `login_status`, stale instrumentation, a second client, missing process/DB/INI cleanup, reused screenshot, non-120 capture tick, non-30-Hz logical delta evidence, or differing runtime positions is fatal.

The final manifest contains distinct paths/SHA-256 for:

```text
original-223325-278475-1920x1080.png
ue-223325-278475-1920x1080.png
side-by-side-223325-278475-1920x1080.png
```

The original HUD mask is applied only to image-difference reporting; raw captures are retained. Numerical placement/projection gates remain mandatory, and a human visual check of terrain, fountain, beds, benches, lamps, stairs, and absence of false palace/bank meshes is recorded in the report.

- [ ] **Step 8: Verify GREEN, inspect both screenshots, and commit**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_scene_parity_check -v
# Focused FreeType build-contract GREEN; expected: Ran 6 tests, OK, no skips.
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_command_plan_requires_freetype_before_solution_game \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_command_plan_rejects_freetype_after_solution_game \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_requires_all_freetype_input_hashes \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_rejects_missing_or_changed_freetype_resource_input \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_rejects_stale_freetype_input_or_toolchain \
  scripts.tests.test_capture_garner_parity.OriginalBuildContractTests.test_attestation_rejects_missing_or_stale_freetype_output -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_rebuild_garner_scene \
  CorsairsUE.Scripts.tests.test_legacy_scene_cleanup \
  CorsairsUE.Scripts.tests.test_scene_parity_material_modes \
  scripts.tests.test_capture_garner_parity -v
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex -MaxParallelActions=2
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.SceneParity" \
  -TestExit="Automation Test Queue Empty"
python3 scripts/build_garner_reference.py --bootstrap-if-missing
python3 scripts/build_garner_reference.py
# Run on the Windows VS 2026 Preview build host before the Mac capture:
pwsh -File scripts/build-original-parity-client.ps1
python3 scripts/capture_garner_parity.py \
  --db-host 127.0.0.1 \
  --account admin \
  --character Test195126 \
  --contract CorsairsUE/Data/Capture/garner_reference_capture.json \
  --output artifacts/scene-parity/capture
git add \
  CorsairsUE/Scripts/scene_parity_check.py \
  CorsairsUE/Scripts/check_scene_parity.py \
  CorsairsUE/Scripts/export_scene_parity_runtime.py \
  CorsairsUE/Scripts/legacy_scene_cleanup.py \
  CorsairsUE/Scripts/configure_garner_scene_base.py \
  CorsairsUE/Scripts/rebuild_garner_scene.py \
  CorsairsUE/Scripts/atomic_publish_garner.py \
  CorsairsUE/Scripts/capture_scene_parity_material_modes.py \
  CorsairsUE/Scripts/tests/test_scene_parity_check.py \
  CorsairsUE/Scripts/tests/test_legacy_scene_cleanup.py \
  CorsairsUE/Scripts/tests/test_rebuild_garner_scene.py \
  CorsairsUE/Scripts/tests/test_scene_parity_material_modes.py \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsParityCaptureSubsystem.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsParityCaptureSubsystem.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsParityCaptureTests.cpp \
  CorsairsUE/Data/Capture/garner_reference_capture.json \
  CorsairsUE/Data/Capture/scene_subtractive_capture.json \
  CorsairsUE/Scripts/place_objects.py \
  CorsairsUE/Scripts/place_all_maps.py \
  CorsairsUE/Scripts/check_placement.py \
  scripts/build_garner_reference.py \
  scripts/build-original-parity-client.ps1 \
  scripts/capture_garner_parity.py \
  scripts/tests/test_capture_garner_parity.py \
  scripts/dev/run-original-client.sh \
  sources/Client/src/App/OriginalParityCapture.h \
  sources/Client/src/App/OriginalParityCapture.cpp \
  sources/Client/src/App/Main.cpp \
  sources/Client/src/App/GameAppInterface.cpp \
  sources/Client/Game.vcxproj \
  tools/AssetConverter/src/Main.cpp
git commit -m "feat(ue): rebuild Garner scene deterministically"
```

## Dependency Order

```text
Scene Task 1
Terrain Task 1 -> Scene Task 2
Scene Tasks 1 + 2 -> Scene Task 4
Scene Task 4 -> Scene Task 3 real reference-model gate
Scene Tasks 3 + 4 -> Scene Task 5
Scene Tasks 4 + 5 -> Scene Task 6
Scene Tasks 3 + 6 -> Scene Task 7 real import + separate synthetic fixture import
Garner terrain plan Tasks 7 + 8 -> tracked terrain manifest/base builder/import/checker
Scene Task 7 + tracked terrain base -> Scene Task 8
```

Task 8 is complete only after a clean-checkout-capable tracked terrain/GameMode base gate and the fixed 1920×1080 original/UE parity capture exist beside their manifests and side-by-side image. A visually improved but numerically failing placement, or a run that depended on ignored local `Content`, is not accepted.
