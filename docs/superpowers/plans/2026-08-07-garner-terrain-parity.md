# Garner Terrain Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the single-dominant-texture Garner terrain around `(223325,278475)` with a deterministic page whose geometry, four legacy texture layers, alpha masks, RGB565 tint, and memory use match the original client.

**Architecture:** Keep the existing span-based `ParseMap` only for small compatibility fixtures. Add a seekable section reader for production, stream full-map rasters and PNG rows, bake one bounded 128×128-cell reference page, generate an adaptive page mesh, and import the page through an idempotent headless Unreal pipeline. Treat every budget, missing input, and dominant-material overlap as a fail-fast gate.

**Tech Stack:** C++23, CMake 3.20, zlib, in-tree SQLite and stb_image, glTF 2.0, Python 3 `unittest`, Unreal Engine 5.8.1 C++/Python, UE Automation Tests.

## Global Constraints

- Work only in `/Users/ivan/code/TalesOfPirate/codex-ue-parity` on branch `qwertyhq/codex-ue-parity`.
- The approved design is `docs/superpowers/specs/2026-08-07-garner-reference-zone-parity-design.md`.
- Follow strict TDD for every task: add the smallest failing test, run it and record the expected failure, then write production code and rerun the focused and full suites.
- Do not modify or stage `databases/game.db`, generated `Content`, `artifacts`, `Intermediate`, `Saved`, `Binaries`, `__pycache__`, or `Client/user/system.ini`. Tracked commands in Tasks 7–8 are allowed to regenerate ignored `artifacts` and `Content`; never add those outputs to git or hand-edit them.
- `MapTerrain::Tiles` and `ReadWholeFile` remain compatibility-only and must not be reachable from the production `terrain-reference` command.
- The reference page is `(17,21)`, covers source cells `[2176,2304) × [2688,2816)`, and uses 32 texels per one-meter cell.
- Page `(17,21)` and camera-frustum cells `[2193,2273) × [2756,2803)` must contain zero absent sections and zero unresolved used texture layers.
- Peak RSS must be at most 128 MiB, decoded texture cache at most 32 MiB, RGBA staging at most one 16 KiB row, and the final 4096×4096 PNG at most 96 MiB.
- Terrain ambient is exactly `(1,1,1)` with `dwTColor=0`; never apply the scene-object ambient factor `0.6`.
- Layer resolution stops at the first upper-layer `textureId=0`. Alpha ID 0 is a no-op; alpha IDs 1 through 15 select literal atlas rectangles.
- Imported reference material is masked and unlit, sends the baked RGB to emissive and alpha to opacity mask, keeps texture streaming enabled, and has explicit StaticMesh and Nanite usage.
- Legacy dominant-material scripts must reject `/Game/Terrain/Reference`; they must never silently overwrite the reference page.
- Run heavy builds, converter probes, Unreal automation, and captures strictly
  sequentially; never overlap them across tasks or agents. CMake build
  commands use at most `-j4`; do not raise the parallelism or launch a second
  heavy command while one is active.
- Every `Game.exe`, CrossOver/Wine, `UnrealEditor`, or `UnrealEditor-Cmd`
  process started by a task is foreground-bound or has its exact PID recorded
  by that task. Terminate and `wait` for those owned PIDs immediately after
  the active test/capture on success, failure, timeout, or interruption; use
  a scoped cleanup trap where a wrapper starts child processes. Never leave
  these processes idle between commands, and never use a broad process-name
  kill that could terminate a user-owned session.
- Each GREEN task receives an implementation review and a separate commit before the next task begins.

## Baseline Commands

Run before Task 1, one command at a time in the shown order, and save the
output in the SDD task report:

```bash
cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s CorsairsUE/Scripts/tests -v
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
```

Expected baseline on commit `efcc929f`: converter `1/1`, Python `10/10`, Unreal build `Succeeded`.

---

### Task 1: Read `.map` sections without materializing the whole map

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPage.h`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapSectionReader.h`
- Create: `tools/AssetConverter/src/MapSectionReader.cpp`
- Create: `tools/AssetConverter/tests/TestMapSectionReader.cpp`
- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapParser.h`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct MapCellRect {
    std::uint32_t X{0};
    std::uint32_t Y{0};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
};

struct TerrainPageId {
    std::uint32_t X{0};
    std::uint32_t Y{0};
};

struct MapSection {
    std::uint32_t X{0};
    std::uint32_t Y{0};
    bool Present{false};
    std::vector<MapTile> Tiles;
};

struct MapPageTiles {
    MapCellRect Cells;
    std::uint32_t StoredWidth{0};
    std::uint32_t StoredHeight{0};
    std::vector<MapTile> Tiles;
    std::vector<std::uint8_t> TilePresent;
    std::vector<std::uint8_t> SectionPresent;
};

struct MapReadStats {
    std::uint64_t MetadataBytesRead{0};
    std::size_t LargestMetadataRead{0};
    std::uint64_t BodyBytesRead{0};
    std::size_t LargestBodyRead{0};
    std::size_t PeakResidentTiles{0};
};

class MapSectionReader {
public:
    static std::optional<MapSectionReader> Open(
        const std::filesystem::path& path, MapDiagnostics& diagnostics);
    const MapFileHeader& Header() const noexcept;
    std::optional<MapSection> ReadSection(
        std::uint32_t x, std::uint32_t y, MapDiagnostics& diagnostics);
    std::optional<MapPageTiles> ReadWindow(
        MapCellRect cells,
        std::uint32_t rightHalo,
        std::uint32_t bottomHalo,
        MapDiagnostics& diagnostics);
    const MapReadStats& Stats() const noexcept;
};
```

- [ ] **Step 1: Add RED tests for bounded section I/O**

Create a sparse 4×2 fixture with 2×2 sections. Put four literal `MapTile` records in section `(0,0)`, store offset zero for section `(1,0)`, and add a corrupt fixture whose nonzero offset points past EOF.

The test cases must be named:

```text
MapSectionReader_ReadsPresentAndAbsentSections
MapSectionReader_RejectsAnyNonZeroOffsetOutsideFile
MapSectionReader_ReadsGarnerGoldenCell
MapSectionReader_ReadsBoundedPageAndHalo
```

Assert:

- present section returns exactly four literal tiles;
- absent section returns `Present=false` and no tile body;
- corrupt offset produces `MapStatus::BODY_TRUNCATED`;
- Garner cell `(2233,2784)` resolves through section offset `35564436`, tile byte offset `35564451`, `BaseTex=4`, `TileInfo=0x02cf2000`, `Color=0xffff`, and `Height=6`;
- `ReadWindow({2176,2688,128,128},1,1)` stores `129×129`, reports `LargestBodyRead == 960`, and reports `PeakResidentTiles == 16705`: `16641` output-page tiles plus the current `64`-tile section held during copying. Offset-table reads count only as metadata and do not weaken the body bound.

- [ ] **Step 2: Run focused tests and Verify RED**

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
./tools/AssetConverter/build/AssetConverterTests
```

Expected: compile fails because `MapSectionReader.h` does not exist.

- [ ] **Step 3: Implement validated seek/read**

Read the 20-byte header and complete offset table once. Validate dimensions exactly as `ParseMap` does. Validate every nonzero absolute offset against:

```cpp
if (sectionBytes > fileBytes ||
    offset < prefixBytes ||
    static_cast<std::uint64_t>(offset) > fileBytes - sectionBytes) {
    diagnostics.Status = MapStatus::BODY_TRUNCATED;
    return std::nullopt;
}
```

`ReadSection` performs one bounded seek/read. `ReadWindow` copies only intersecting tiles into its page-plus-halo arrays and records section/tile presence. `PeakResidentTiles` counts simultaneous operation residency, so the window path includes both the output page and the current section. Never allocate `Width * Height` tiles.

- [ ] **Step 4: Verify GREEN and compatibility**

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
./tools/AssetConverter/build/AssetConverterTests
ctest --test-dir tools/AssetConverter/build --output-on-failure
```

Expected: all converter tests pass, including the existing span-based parser tests.

- [ ] **Step 5: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapParser.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPage.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MapSectionReader.h \
  tools/AssetConverter/src/MapSectionReader.cpp \
  tools/AssetConverter/tests/TestMapSectionReader.cpp
git commit -m "feat(converter): stream map sections"
```

---

### Task 2: Stream full-map height, block, and region rasters

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/StreamingMapWriter.h`
- Create: `tools/AssetConverter/src/StreamingMapWriter.cpp`
- Create: `tools/AssetConverter/tests/TestStreamingMapWriter.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct MapRasterStats {
    std::size_t PresentSections{0};
    std::size_t AbsentSections{0};
    std::int8_t MinHeightRaw{0};
    std::int8_t MaxHeightRaw{0};
    std::size_t BlockedTiles{0};
};

MapWriteStatus WriteTerrainRasters(
    MapSectionReader& reader,
    const std::filesystem::path& basePath,
    MapRasterStats& stats,
    std::string& detail);
```

- [ ] **Step 1: Add RED tests**

Use the Task 1 sparse fixture and require:

- exact sizes `gridWidth * gridHeight * 2`, `* 4`, and `* 2`;
- raw height `6` becomes little-endian bytes `00 86`;
- every byte belonging to the absent section remains zero;
- two writes are byte-identical;
- `reader.Stats().PeakResidentTiles` never exceeds one section plus the writer row;
- no overload accepts `MapTerrain`, preventing accidental full-map production use.

- [ ] **Step 2: Verify RED**

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
./tools/AssetConverter/build/AssetConverterTests
```

Expected compile error: `StreamingMapWriter.h` is absent.

- [ ] **Step 3: Implement seek-based raster output**

Pre-size the three files, then iterate source sections in deterministic row-major order. For each present section, encode one source row and write it at its final raster offset with `seekp`. Do not rewrite absent ranges. Emit the same metadata fields as `WriteTerrain`, using the header dimensions and collected statistics.

- [ ] **Step 4: Verify GREEN**

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/StreamingMapWriter.h \
  tools/AssetConverter/src/StreamingMapWriter.cpp \
  tools/AssetConverter/tests/TestStreamingMapWriter.cpp
git commit -m "feat(converter): stream terrain rasters"
```

---

### Task 3: Resolve legacy layers and source textures canonically

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainLayers.h`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainCatalog.h`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainTextureCache.h`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainTextureSampling.h`
- Create: `tools/AssetConverter/src/TerrainLayers.cpp`
- Create: `tools/AssetConverter/src/TerrainCatalog.cpp`
- Create: `tools/AssetConverter/src/TerrainTextureCache.cpp`
- Create: `tools/AssetConverter/src/TerrainTextureSampling.cpp`
- Create: `tools/AssetConverter/src/StbImageDecoder.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainLayers.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainCatalog.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainTextureSampling.cpp`
- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ImageCodec.h`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct TerrainLayer {
    std::uint8_t TextureId{0};
    std::uint8_t AlphaMask{0};
};

struct ResolvedTerrainLayers {
    std::array<TerrainLayer, 4> Values{};
    std::size_t Count{0};
};

ResolvedTerrainLayers ResolveTerrainLayers(const MapTile& tile) noexcept;

struct AtlasRect {
    float U0{0};
    float V0{0};
    float U1{0};
    float V1{0};
};

std::optional<AtlasRect> ResolveAlphaAtlasRect(
    std::uint8_t mask) noexcept;

class TerrainCatalog {
public:
    static std::optional<TerrainCatalog> Load(
        const std::filesystem::path& sqlitePath,
        const std::filesystem::path& clientRoot,
        std::string& detail);
    std::optional<std::filesystem::path> Resolve(
        std::uint8_t textureId) const;
};

class TerrainTextureCache {
public:
    explicit TerrainTextureCache(
        std::size_t maxDecodedBytes);
    std::shared_ptr<const DecodedImage> Load(
        const std::filesystem::path& path,
        std::string& detail);
    std::size_t DecodedBytes() const noexcept;
    std::size_t PeakDecodedBytes() const noexcept;
    std::size_t EntryCount() const noexcept;
};

std::optional<DecodedImage> DecodeImageFile(
    const std::filesystem::path& path, std::string& detail);

enum class TerrainAddressMode : std::uint32_t {
    WRAP,
    MIRROR,
};

std::array<std::uint8_t, 4> SampleTerrainImageLinear(
    const DecodedImage& image,
    double normalizedU,
    double normalizedV,
    TerrainAddressMode addressU,
    TerrainAddressMode addressV);
```

- [ ] **Step 1: Add RED layer tests**

Literal expectations:

- Garner tile `BaseTex=4`, `TileInfo=0x02cf2000` resolves only `{4,15}`; the encoded `{15,2}` after the first zero texture ID is stale and must not appear.
- `BaseTex=0` returns `Count=0`.
- Synthetic `{4/15, 7/0, 8/15, 0}` returns `{4/15, 8/15}`: alpha zero skips a layer but is not the texture-ID terminator.
- Alpha 0 returns `nullopt`.
- Alpha rectangles are `1 -> {0,0,.25,.25}`, `14 -> {.25,.75,.5,1}`, and `15 -> {.5,.75,.75,1}`; sampling applies a literal 0.01 inset inside the chosen rectangle.

- [ ] **Step 2: Add RED catalog/cache tests**

Load tracked `databases/gamedata.sqlite`. Assert:

```text
4 -> Client/texture/terrain/brick05.png
5 -> Client/texture/terrain/grass05.png
```

Decode `brick05.png` and `terrain/alpha/total.png` as 256×256 RGBA. Exercise LRU eviction and assert both `DecodedBytes()` and `PeakDecodedBytes()` never exceed `32 * 1024 * 1024`; retaining a returned `shared_ptr` must keep that image valid after eviction from the lookup map.

The DB stores `texture/terrain/brick05.bmp` and `grass05.bmp`, while the tracked converted sources are `.png`. The only normalization is: validate a relative path with no traversal, require a case-insensitive final `.bmp`, replace only that final extension with `.png`, then require the resulting path to remain under `clientRoot` and exist. Any other extension, missing PNG, absolute path, or `..` is fatal. Tests require the literal DB names above to resolve exactly to the two PNG paths and reject `brick05.dds`, traversal, and missing PNG.

Add an asymmetric 4×4 RGBA fixture whose red channel is row-major `0..15`. The sampler uses D3D9 level-0 linear semantics: texel centers are `(i+0.5)/size`, so normalized coordinates map to `s=u*width-0.5`, `t=v*height-0.5`; address integer taps before interpolation; round the final UNORM channel with `floor(value+0.5)`. Require:

```text
WRAP at (0,0) -> average of source texels 15,12,3,0 -> red 8
MIRROR at (0,0) -> source texel 0 -> red 0
MIRROR at (0.5/4,0.5/4) -> top-left texel 0, not bottom-left 12
WRAP at (3.5/4,0.5/4) -> top-right texel 3
```

This fixes `V=0` to the first decoded image row; no implicit image flip is allowed.

- [ ] **Step 3: Verify RED**

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
./tools/AssetConverter/build/AssetConverterTests
```

Expected compile failure on the new headers.

- [ ] **Step 4: Implement resolver, catalog, decoder, and bounded LRU**

Change CMake to `project(CorsairsAssetConverter LANGUAGES C CXX)`, compile the tracked SQLite C source, and compile one stb_image implementation translation unit. Apply only the tested final `.bmp` to `.png` source normalization; fail if a used ID has no readable source file.

`SampleTerrainImageLinear` exactly implements the tested texel-center, top-row V orientation, WRAP, and MIRROR rules. It does not use nearest filtering, implicit vertical flip, or platform image-library sampling.

- [ ] **Step 5: Verify GREEN**

```bash
cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ImageCodec.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainLayers.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainCatalog.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainTextureCache.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainTextureSampling.h \
  tools/AssetConverter/src/TerrainLayers.cpp \
  tools/AssetConverter/src/TerrainCatalog.cpp \
  tools/AssetConverter/src/TerrainTextureCache.cpp \
  tools/AssetConverter/src/TerrainTextureSampling.cpp \
  tools/AssetConverter/src/StbImageDecoder.cpp \
  tools/AssetConverter/tests/TestTerrainLayers.cpp \
  tools/AssetConverter/tests/TestTerrainCatalog.cpp \
  tools/AssetConverter/tests/TestTerrainTextureSampling.cpp
git commit -m "feat(converter): resolve legacy terrain layers"
```

---

### Task 4: Write PNG rows through compressed streaming zlib

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/StreamingPngWriter.h`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/Sha256.h`
- Create: `tools/AssetConverter/src/StreamingPngWriter.cpp`
- Create: `tools/AssetConverter/src/Sha256.cpp`
- Create: `tools/AssetConverter/tests/TestStreamingPngWriter.cpp`
- Modify: `tools/AssetConverter/src/PngWriter.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
class StreamingPngWriter {
public:
    static std::unique_ptr<StreamingPngWriter> Open(
        const std::filesystem::path& path,
        std::uint32_t width,
        std::uint32_t height,
        std::string& detail);
    bool WriteRgbaRow(
        std::span<const std::uint8_t> row, std::string& detail);
    bool Finish(std::string& detail);
    std::size_t PeakRgbaRowBytes() const noexcept;
};

std::string Sha256Bytes(
    std::span<const std::uint8_t> bytes);

std::optional<std::string> Sha256File(
    const std::filesystem::path& path,
    std::string& detail);
```

- [ ] **Step 1: Add RED tests**

Write red/green and blue/white rows to a 2×2 image, decode it with `DecodeImageFile`, and compare all 16 RGBA bytes. Assert raw-pixel SHA-256:

```text
c21b35e3f28e676cedf24c13575a7346682e101a2d26aad9598d0cdbcee9ee3b
```

Also require deterministic PNG bytes, rejection of a wrong row length, rejection of an extra row, rejection of `Finish` before all rows, and `PeakRgbaRowBytes == 16384` for a 4096-pixel row.

- [ ] **Step 2: Verify RED**

```bash
nice -n 10 cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
nice -n 10 ./tools/AssetConverter/build/AssetConverterTests
```

Expected compile failure because the streaming writer is absent.

- [ ] **Step 3: Implement zlib stream**

Add `find_package(ZLIB REQUIRED)` and link `ZLIB::ZLIB`. `Open` writes PNG signature and IHDR. Each row is filtered and immediately passed into `deflate`; flush IDAT chunks no larger than 64 KiB. Keep only one source row and one bounded compressed chunk. Make existing `WritePng` a compatibility adapter over the streaming writer.

- [ ] **Step 4: Verify GREEN**

```bash
nice -n 10 cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
nice -n 10 cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
nice -n 10 ctest --test-dir tools/AssetConverter/build --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/StreamingPngWriter.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/Sha256.h \
  tools/AssetConverter/src/StreamingPngWriter.cpp \
  tools/AssetConverter/src/Sha256.cpp \
  tools/AssetConverter/src/PngWriter.cpp \
  tools/AssetConverter/tests/TestStreamingPngWriter.cpp
git commit -m "feat(converter): stream compressed png rows"
```

---

### Task 5: Bake the canonical Garner reference albedo page

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPageBaker.h`
- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ProcessMetrics.h`
- Create: `tools/AssetConverter/src/TerrainPageBaker.cpp`
- Create: `tools/AssetConverter/src/ProcessMetrics.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainPageBaker.cpp`
- Create: `tools/AssetConverter/tests/TerrainPageBudgetProbe.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct TerrainBakeOptions {
    std::uint32_t CellsPerPage{128};
    std::uint32_t PixelsPerCell{32};
    std::size_t MaxTextureCacheBytes{32u * 1024u * 1024u};
    std::size_t MaxRssBytes{128u * 1024u * 1024u};
    std::size_t MaxPngBytes{96u * 1024u * 1024u};
};

struct TerrainBakeResult {
    bool Ok{false};
    std::string AlgorithmVersion{
        "legacy-fixed-pipeline-v1"};
    MapCellRect SourceCellBounds{};
    std::filesystem::path PngPath;
    std::string PngSha256;
    std::vector<std::uint8_t> UsedTextureIds;
    std::uint32_t SectionOriginX{0};
    std::uint32_t SectionOriginY{0};
    std::uint32_t SectionGridWidth{0};
    std::uint32_t SectionGridHeight{0};
    std::vector<std::uint8_t> SectionPresenceMask;
    std::size_t AbsentSections{0};
    std::size_t UnresolvedLayers{0};
    std::size_t PeakRssBytes{0};
    std::size_t OutputBytes{0};
    std::size_t PeakTextureCacheBytes{0};
    std::size_t PeakRgbaRowBytes{0};
};

TerrainBakeResult BakeTerrainPage(
    MapSectionReader& reader,
    const TerrainCatalog& catalog,
    TerrainPageId page,
    const std::filesystem::path& alphaAtlas,
    const std::filesystem::path& outputDirectory,
    const TerrainBakeOptions& options,
    std::string& detail);
```

- [ ] **Step 1: Add RED pure-pixel tests**

Assert:

- texture UV at source cell 0 equals cell 4;
- RGB565 values `0xf800`, `0x07e0`, `0x001f`, and `0xffff` expand to literal full-range red, green, blue, and white;
- red base plus blue layer at alpha 128 produces `{127,0,128,255}`;
- a synthetic four-color 2×2 page with `PixelsPerCell=1` decodes to the Task 4 SHA-256;
- non-coplanar RGB565 corner interpolation follows the two source triangles, not bilinear interpolation.
- an asymmetric texture/alpha fixture produces literal corner/interior pixels that differ under nearest filtering, V flip, WRAP-vs-MIRROR, or missing texel-center offset.

- [ ] **Step 2: Add RED real-Garner gates**

Assert:

- source cell `(2233,2784)` uses only texture ID 4, `brick05`;
- page `(17,21)` and the required frustum rectangle have no absent sections;
- result source bounds are exactly `{2176,2688,128,128}` and its row-major section-presence mask covers the exact page section grid with every value 1;
- no used layer is unresolved;
- two bakes have identical PNG SHA-256;
- the in-process unit test requires a 4096×4096 output no larger than 96 MiB, `PeakTextureCacheBytes <= 32 MiB`, and `PeakRgbaRowBytes <= 16 KiB`; it does not claim an RSS bound because the aggregate `AssetConverterTests` process also runs compatibility tests that materialize full Garner.

Add the exact CMake target and CTest:

```cmake
add_executable(
    TerrainPageBudgetProbe
    tests/TerrainPageBudgetProbe.cpp)
target_link_libraries(
    TerrainPageBudgetProbe
    PRIVATE AssetConverterLib)
add_test(
    NAME TerrainPageBudget
    COMMAND TerrainPageBudgetProbe
        --repo-root
        ${CORSAIRS_REPO_ROOT})
set_tests_properties(
    TerrainPageBudget
    PROPERTIES
    WORKING_DIRECTORY
        ${CORSAIRS_REPO_ROOT})
```

`AssetConverterLib` is the existing shared production library target already used by `AssetConverter` and `AssetConverterTests`; the probe links it instead of compiling a divergent implementation. `TerrainPageBudgetProbe` requires the explicit `--repo-root` argument, resolves every real input beneath that root, performs only one production bake in a fresh process, and exits nonzero unless peak RSS is at most 128 MiB, PNG size at most 96 MiB, texture cache at most 32 MiB, and row staging at most 16 KiB.

Focused in-process tests set `MaxRssBytes=std::numeric_limits<std::size_t>::max()` and assert the recorded value only. The standalone budget probe and production command keep the 128 MiB option and are the only tests that verdict RSS.

- [ ] **Step 3: Verify RED**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverterTests TerrainPageBudgetProbe -j4
```

Expected compile error on `TerrainPageBaker.h`.

- [ ] **Step 4: Implement fixed-pipeline bake**

For each output pixel:

1. derive source cell and pixel-center coordinates `localU=(pixelInCellX+0.5)/PixelsPerCell`, `localV=(pixelInCellY+0.5)/PixelsPerCell`;
2. sample base texture with `u=((cellX mod 4)+localU)/4`, `v=((cellY mod 4)+localV)/4`, level-0 linear filtering, top-row V orientation, and WRAP on both axes;
3. for each upper layer, sample its terrain texture with the same coordinates/WRAP; sample the alpha atlas with `u=rectU0+0.01+localU*(0.25-0.02)`, `v=rectV0+0.01+localV*(0.25-0.02)`, linear filtering, and MIRROR on both axes; overlay in source order with the sampled atlas alpha;
4. triangle-interpolate RGB565 tint;
5. compute `legacyDiffuse = saturate(1.0 * RGB565 + 0)`;
6. multiply the composite by diffuse and stream the RGBA row.

Collect sorted used IDs, hashes, absent/unresolved counts, RSS, and output bytes. Fail before publishing a final manifest if any gate is violated.

- [ ] **Step 5: Verify GREEN**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests TerrainPageBudgetProbe -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
ctest --test-dir tools/AssetConverter/build \
  --output-on-failure -R '^TerrainPageBudget$'
"$PWD/tools/AssetConverter/build/TerrainPageBudgetProbe" \
  --repo-root "$PWD"
```

- [ ] **Step 6: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPageBaker.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ProcessMetrics.h \
  tools/AssetConverter/src/TerrainPageBaker.cpp \
  tools/AssetConverter/src/ProcessMetrics.cpp \
  tools/AssetConverter/tests/TestTerrainPageBaker.cpp \
  tools/AssetConverter/tests/TerrainPageBudgetProbe.cpp
git commit -m "feat(converter): bake Garner terrain page"
```

---

### Task 6: Generate an adaptive reference-page mesh with error gates

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h`
- Create: `tools/AssetConverter/src/TerrainPageMeshWriter.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainPageMeshWriter.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

**Interfaces:**

```cpp
struct TerrainMeshError {
    double MaxAbsCm{0};
    double RmsCm{0};
    double SharedBoundaryMaxCm{0};
    std::size_t Samples{0};
};

TerrainMeshError EvaluateTerrainPageStep(
    const MapPageTiles& page, std::uint32_t step);

struct TerrainPageMeshOptions {
    double MaxAbsCm{5.0};
    double MaxRmsCm{2.0};
    double MaxSharedBoundaryCm{0.0};
};

struct TerrainPageMeshResult {
    bool Ok{false};
    std::uint32_t Step{0};
    TerrainMeshError Error;
    std::filesystem::path GltfPath;
    std::filesystem::path BinPath;
    double ActorWorldXcm{0};
    double ActorWorldYcm{0};
};

TerrainPageMeshResult WriteTerrainPageMesh(
    const MapPageTiles& page,
    TerrainPageId pageId,
    const std::filesystem::path& outputDirectory,
    const TerrainPageMeshOptions& options,
    std::string& detail);
```

- [ ] **Step 1: Add RED tests**

Require:

- planar page chooses step 4 with all errors zero;
- a 100 cm central spike rejects steps 4 and 2 and chooses step 1;
- neighboring pages with different interior steps have zero shared-boundary error;
- page `(17,21)` chooses a step whose max error is at most 5 cm, RMS at most 2 cm, and seam exactly zero;
- emitted glTF UV corners are `(0,0)` and `(1,1)`, and winding is top-facing;
- page `(17,21)` vertices are local: X `[0,128] m`, glTF/source-map Y `[0,-128] m`; its UE actor transform is exactly `(217600,-268800,0) cm`, producing world bounds X `[217600,230400]` and Y `[-281600,-268800]` cm. Absolute source coordinates inside both the mesh and actor transform are a failure.

- [ ] **Step 2: Verify RED**

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
./tools/AssetConverter/build/AssetConverterTests
```

Expected compile error on the new writer.

- [ ] **Step 3: Implement adaptive evaluation and mesh output**

Evaluate candidates in order `{4,2,1}` and choose the first passing candidate. Preserve a full-resolution boundary ring even when the interior is decimated. Compare every original per-cell sample against the generated source triangles. Keep the old `WriteTerrainMesh(MapTerrain)` unchanged and compatibility-only.

- [ ] **Step 4: Verify GREEN**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverterTests TerrainPageBudgetProbe -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h \
  tools/AssetConverter/src/TerrainPageMeshWriter.cpp \
  tools/AssetConverter/tests/TestTerrainPageMeshWriter.cpp
git commit -m "feat(converter): generate bounded terrain page mesh"
```

---

### Task 7: Expose one fail-fast production terrain command

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h`
- Create: `tools/AssetConverter/src/TerrainReferenceCommand.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainReferenceCommand.cpp`
- Create: `CorsairsUE/Scripts/install_runtime_map_data.py`
- Create: `CorsairsUE/Scripts/tests/test_install_runtime_map_data.py`
- Modify: `tools/AssetConverter/src/Main.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`
- Modify: `.gitignore`

**Interfaces:**

```cpp
struct TerrainReferenceOptions {
    std::filesystem::path Map;
    std::filesystem::path Database;
    std::filesystem::path ClientRoot;
    std::filesystem::path AlphaAtlas;
    std::filesystem::path Output;
    TerrainPageId Page{17, 21};
    MapCellRect RequiredPresent{2193, 2756, 80, 47};
    TerrainBakeOptions Bake{};
    TerrainPageMeshOptions Mesh{};
};

enum class TerrainManifestIssueCode : std::uint32_t {
    INVALID_SCHEMA,
    INVALID_ALGORITHM,
    INVALID_BOUNDS,
    INVALID_TEXTURE_IDS,
    INVALID_SECTION_MASK,
    INVALID_FILE,
    INVALID_HASH,
    INVALID_METRIC,
    BUDGET_EXCEEDED,
};

struct TerrainOutputFile {
    std::filesystem::path ManifestRelativePath;
    std::string Sha256;
};

struct TerrainManifestSourceDto {
    std::filesystem::path MapPath;
    std::string MapSha256;
};

struct TerrainManifestPageDto {
    TerrainPageId Id;
    MapCellRect SourceCellBounds;
    std::uint32_t PixelsPerCell;
    std::uint32_t PixelWidth;
    std::uint32_t PixelHeight;
    std::array<double, 3> Ambient;
    std::uint32_t DwTColor;
};

struct TerrainManifestSectionPresenceDto {
    std::uint32_t OriginX;
    std::uint32_t OriginY;
    std::uint32_t Width;
    std::uint32_t Height;
    std::vector<std::uint8_t> RowMajorMask;
};

struct TerrainManifestFilesDto {
    TerrainOutputFile Height;
    TerrainOutputFile Block;
    TerrainOutputFile Region;
    TerrainOutputFile TerrainMetadata;
    TerrainOutputFile Albedo;
    TerrainOutputFile MeshGltf;
    TerrainOutputFile MeshBin;
};

struct TerrainManifestMetricsDto {
    std::uint64_t PeakRssBytes;
    std::uint64_t PeakTextureCacheBytes;
    std::uint64_t PeakRgbaRowBytes;
    std::uint64_t PngBytes;
    std::uint64_t TotalOutputBytes;
    double MaxHeightErrorCm;
    double RmsHeightErrorCm;
    double SharedBoundaryMaxCm;
    std::uint64_t AbsentSectionCount;
    std::uint64_t UnresolvedLayerCount;
};

struct TerrainReferenceManifestDto {
    std::uint32_t SchemaVersion;
    std::string AlgorithmVersion;
    TerrainManifestSourceDto Source;
    TerrainManifestPageDto Page;
    MapCellRect RequiredPresentRect;
    std::vector<std::uint8_t> UsedTextureIds;
    TerrainManifestSectionPresenceDto SectionPresence;
    TerrainManifestFilesDto Files;
    TerrainManifestMetricsDto Metrics;
};

struct TerrainManifestIssue {
    TerrainManifestIssueCode Code;
    std::string Field;
    std::string Detail;
};

std::vector<TerrainManifestIssue>
ValidateTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest,
    const std::filesystem::path& manifestDirectory,
    const TerrainReferenceOptions& limits);

std::optional<TerrainReferenceManifestDto>
ParseTerrainReferenceManifest(
    std::string_view json,
    std::vector<TerrainManifestIssue>& issues);

struct TerrainBuiltFile {
    std::filesystem::path RunLeafPath;
    std::string Sha256;
};

struct TerrainBuildFiles {
    TerrainBuiltFile Height;
    TerrainBuiltFile Block;
    TerrainBuiltFile Region;
    TerrainBuiltFile TerrainMetadata;
    TerrainBuiltFile Albedo;
    TerrainBuiltFile MeshGltf;
    TerrainBuiltFile MeshBin;
};

struct TerrainReferenceBuildProducts {
    TerrainBakeResult Bake;
    TerrainPageMeshResult Mesh;
    TerrainBuildFiles Files;
};

struct TerrainReferenceDependencies {
    std::function<std::optional<TerrainReferenceBuildProducts>(
        const TerrainReferenceOptions&,
        const std::filesystem::path& runDirectory,
        std::string& detail)> BuildProducts;
    std::function<bool(
        const std::filesystem::path& destination,
        std::string_view json,
        std::string& detail)> AtomicPublish;
};

TerrainReferenceDependencies
MakeProductionTerrainReferenceDependencies();

int RunTerrainReference(
    const TerrainReferenceOptions& options,
    const TerrainReferenceDependencies& dependencies,
    std::ostream& output,
    std::ostream& error);

int RunTerrainReference(
    const TerrainReferenceOptions& options,
    std::ostream& output,
    std::ostream& error);
```

`ParseTerrainReferenceManifest` is the only JSON-to-DTO entry point. It
requires every literal key and its exact scalar/object/array type before
constructing the DTO; a missing member is
`{INVALID_SCHEMA, "<json.pointer>", "required field is missing"}`. The
serializer emits exactly the same DTO shape. No bake/mesh result object is
serialized directly and no required manifest field is recovered from a
default value.

The three-argument `RunTerrainReference` constructs
`MakeProductionTerrainReferenceDependencies()` and delegates to the
injectable overload. The production `BuildProducts` owns the real
section-reader/raster/bake/mesh calls. `AtomicPublish` is the only callback
allowed to replace the top-level manifest; tests replace it with a counting
spy. Neither callback may bypass DTO construction, serialization,
parse-roundtrip, or `ValidateTerrainReferenceManifest` inside
`RunTerrainReference`.

There is one path base everywhere: `manifestDirectory`, the parent of the
published top-level `garner.reference-albedo.json` (normally
`artifacts/maps`). Every serialized `files.*.path` is exactly
`runs/<run-id>/<expected-leaf-name>`, uses forward slashes, and is resolved as
`manifestDirectory / path`. All seven entries must share the same single
`run-id`; absolute paths, `..`, leaf-only paths, a second `runs` segment, and
paths escaping through symlinks are invalid. `TerrainBuiltFile::RunLeafPath`
must be one expected filename with no parent component and is used only while
production files are being written inside `runDirectory`. Before DTO
validation/publication, `RunTerrainReference` rewrites each build-product
leaf to its distinct `TerrainOutputFile::ManifestRelativePath` canonical
top-manifest-relative `runs/<run-id>/<leaf>` value.

- [ ] **Step 1: Add RED orchestration tests**

Use temporary fixtures to require exit code 1 and no newly published manifest for a corrupt offset, missing used texture, absent required section, PNG budget violation, RSS budget violation, or geometry error violation.

Build a valid `TerrainReferenceManifestDto`, serialize it, then use a
table-driven JSON mutation test to delete every required key one at a time
and to replace every scalar/array/object with the wrong JSON type.
`ParseTerrainReferenceManifest` must return `nullopt` and the exact JSON
pointer with `INVALID_SCHEMA`. For DTO-level mutations require the exact
field/code for wrong schema/algorithm/bounds/texture ordering/section
mask/file/hash/total bytes/metric. Path mutations start with a valid fixture
whose manifest is `/tmp/maps/garner.reference-albedo.json`, run directory is
`/tmp/maps/runs/run-001`, and seven paths are
`runs/run-001/<expected-leaf>`. Require `INVALID_FILE` for a leaf-only path,
different run IDs, `runs/run-001/runs/...`, absolute/traversing paths, a
symlink escape, or validation against the run directory instead of
`/tmp/maps`.

Add literal injectable-path tests named:

```text
TerrainReferenceCommand_RejectsInjectedInvalidBakeBeforePublish
TerrainReferenceCommand_RejectsInjectedInvalidMeshBeforePublish
TerrainReferenceCommand_RejectsSerializedRoundTripBeforePublish
TerrainReferenceCommand_PublishesValidatedDtoExactlyOnce
```

Each test passes `TerrainReferenceDependencies` whose `BuildProducts`
returns real temporary files plus `Bake.Ok=true` and `Mesh.Ok=true`. Mutate
one supposedly successful result at a time: wrong bake bounds, one absent
section, one unresolved layer, wrong PNG size/hash, max/RMS/shared-boundary
error over the limits, or a mismatched glTF/bin path. The injected
`AtomicPublish` increments `publishCalls` and captures bytes without touching
disk. Every invalid case requires exit code 1, `publishCalls == 0`, and an
existing top manifest to remain byte-identical. The valid case requires exit
code 0, `publishCalls == 1`, then parses the captured bytes and validates them
against the top manifest directory. This proves validation and serialized
parse-roundtrip happen inside `RunTerrainReference` before publication rather
than only in the production builder callback.

Require a successful run directory to contain these seven leaf files:

```text
garner.height.r16
garner.block.raw
garner.region.raw
garner.terrain.json
garner.albedo_17_21.png
garner.terrain_17_21.gltf
garner.terrain_17_21.bin
```

The eighth output is only the published top-level
`artifacts/maps/garner.reference-albedo.json`; no run-internal manifest is a
consumer input.

Require this literal schema shape (hash/path values abbreviated only here):

```json
{
  "schemaVersion": 1,
  "algorithmVersion": "legacy-fixed-pipeline-v1",
  "source": {
    "mapPath": "Client/map/garner.map",
    "mapSha256": "<sha256>"
  },
  "page": {
    "x": 17,
    "y": 21,
    "sourceCellBounds": {
      "x": 2176,
      "y": 2688,
      "width": 128,
      "height": 128
    },
    "pixelsPerCell": 32,
    "pixelWidth": 4096,
    "pixelHeight": 4096,
    "ambient": [1.0, 1.0, 1.0],
    "dwTColor": 0
  },
  "requiredPresentRect": {
    "x": 2193,
    "y": 2756,
    "width": 80,
    "height": 47
  },
  "usedTextureIds": [4, 5],
  "sectionPresence": {
    "originX": 272,
    "originY": 336,
    "width": 16,
    "height": 16,
    "rowMajorMask": [1]
  },
  "files": {
    "height": {"path": "runs/run-001/garner.height.r16", "sha256": "<sha256>"},
    "block": {"path": "runs/run-001/garner.block.raw", "sha256": "<sha256>"},
    "region": {"path": "runs/run-001/garner.region.raw", "sha256": "<sha256>"},
    "terrainMetadata": {"path": "runs/run-001/garner.terrain.json", "sha256": "<sha256>"},
    "albedo": {"path": "runs/run-001/garner.albedo_17_21.png", "sha256": "<sha256>"},
    "meshGltf": {"path": "runs/run-001/garner.terrain_17_21.gltf", "sha256": "<sha256>"},
    "meshBin": {"path": "runs/run-001/garner.terrain_17_21.bin", "sha256": "<sha256>"}
  },
  "metrics": {
    "peakRssBytes": 0,
    "peakTextureCacheBytes": 0,
    "peakRgbaRowBytes": 0,
    "pngBytes": 0,
    "totalOutputBytes": 0,
    "maxHeightErrorCm": 0.0,
    "rmsHeightErrorCm": 0.0,
    "sharedBoundaryMaxCm": 0.0,
    "absentSectionCount": 0,
    "unresolvedLayerCount": 0
  }
}
```

`usedTextureIds` is the sorted real set, not hard-coded to the illustrative two IDs. `rowMajorMask` length must equal `width*height`; for Garner page `(17,21)` it has exactly 256 ones. RED DTO tests remove or alter every required field, use a wrong algorithm version/bounds/mask length/mask bit/used ID order, and require validation failure; command-path tests require that any issue prevents a newly published manifest.

- [ ] **Step 2: Verify RED**

Expected compile error on the command header, and the installer module is absent:

```bash
cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_install_runtime_map_data -v
```

- [ ] **Step 3: Implement command parsing and atomic publication**

Add this exact invocation:

```bash
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
```

Interpret `--require-present-rect` as `X Y Width Height`, exactly matching `MapCellRect`.

Generate each attempt under `artifacts/maps/runs/<run-id>/`. Production
builders may use leaf paths while writing that private directory, but DTO
construction must serialize all seven paths as
`runs/<run-id>/<expected-leaf>` relative to `artifacts/maps`, the parent of
the top manifest. Call `ValidateTerrainReferenceManifest(dto,
options.Output, options)` with `options.Output == artifacts/maps`; never pass
the run directory. Validate the complete set, including the paired
`.gltf/.bin`, then publish only the small top-level
`artifacts/maps/garner.reference-albedo.json` with an atomic temp-file
rename. The installer and every later consumer resolve the same serialized
path from that top-manifest parent. A crash or failed gate may leave an
unreferenced run directory, but it cannot replace the last valid manifest.

Add a tracked runtime installer:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
```

It rejects paths outside the canonical `runs/<run-id>/<leaf>` grammar,
resolves them only from the top manifest's parent, verifies manifest hashes,
then copies `files.block` and `files.terrainMetadata` through temp files and
rename into `CorsairsUE/Data/Heights`. Add generated
`CorsairsUE/Data/Heights/*.block.raw`, `*.region.raw`, and `*.terrain.json` to
`.gitignore`; existing tracked `.height.r16` files remain untouched. Unit
tests require leaf-only/wrong-base/path-traversal rejection, hash rejection,
and byte-identical repeat installation.

- [ ] **Step 4: Verify GREEN and real command**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests TerrainPageBudgetProbe -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_install_runtime_map_data -v
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
PYTHONDONTWRITEBYTECODE=1 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
```

- [ ] **Step 5: Commit**

```bash
git add \
  .gitignore \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h \
  tools/AssetConverter/src/TerrainReferenceCommand.cpp \
  tools/AssetConverter/src/Main.cpp \
  tools/AssetConverter/tests/TestTerrainReferenceCommand.cpp \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  CorsairsUE/Scripts/tests/test_install_runtime_map_data.py
git commit -m "feat(converter): add Garner terrain reference command"
```

---

### Task 8: Import, assign, and validate the reference page in Unreal

**Files:**

- Create: `CorsairsUE/Scripts/reference_terrain_rules.py`
- Create: `CorsairsUE/Scripts/build_reference_terrain_level.py`
- Create: `CorsairsUE/Scripts/import_reference_terrain.py`
- Create: `CorsairsUE/Scripts/check_reference_terrain.py`
- Create: `CorsairsUE/Scripts/tests/test_reference_terrain_rules.py`
- Create: `CorsairsUE/Scripts/tests/test_module_dependencies.py`
- Create: `CorsairsUE/Source/CorsairsImport/Private/Tests/ReferenceTerrainAssetTests.cpp`
- Modify: `CorsairsUE/Scripts/setup_terrain_material.py`
- Modify: `CorsairsUE/Scripts/apply_terrain_material.py`
- Modify: `CorsairsUE/Scripts/place_terrain.py`
- Modify: `CorsairsUE/Source/CorsairsImport/CorsairsImport.Build.cs`

**Interfaces:**

Pure Python contracts (`reference_terrain_rules.py` imports no `unreal`
module):

```python
from dataclasses import dataclass
from pathlib import Path
from typing import TypedDict


class ValidationIssue(TypedDict):
    code: str
    field: str
    detail: str


REFERENCE_GAME_MODE = "/Script/CorsairsGame.CorsairsGameMode"


@dataclass(frozen=True)
class ReferenceTerrainLimits:
    max_rss_bytes: int = 128 * 1024 * 1024
    max_texture_cache_bytes: int = 32 * 1024 * 1024
    max_rgba_row_bytes: int = 16 * 1024
    max_png_bytes: int = 96 * 1024 * 1024
    max_height_error_cm: float = 5.0
    max_rms_error_cm: float = 2.0
    max_shared_boundary_cm: float = 0.0


def validate_manifest(
    data: dict,
    manifest_path: Path,
    limits: ReferenceTerrainLimits = ReferenceTerrainLimits(),
) -> list[ValidationIssue]: ...


def asset_paths(
    map_name: str,
    page_x: int,
    page_y: int,
) -> dict[str, str]: ...


def reference_actor_object_path(
    map_package: str,
    map_name: str,
    page_x: int,
    page_y: int,
) -> str: ...


def overlapping_legacy_tiles(
    page_bounds: tuple[float, float, float, float],
    actor_bounds:
        list[tuple[str, float, float, float, float]],
) -> list[str]: ...


def assert_legacy_target_allowed(
    entry_point: str,
    asset_path: str,
) -> None: ...


def validate_import_report(data: dict) -> list[ValidationIssue]: ...


def validate_level_build_report(data: dict) -> list[ValidationIssue]: ...


def validate_idempotent_import_reports(
    first: dict,
    second: dict,
) -> list[ValidationIssue]: ...
```

`manifest_path` is required because the only path base is
`manifest_path.parent`, identical to C++ `manifestDirectory`. Validation
requires every entry to match `runs/<one-run-id>/<expected-leaf>`, resolves
that manifest-relative path against the top manifest parent, rejects
absolute paths, `..`, mixed run IDs, nested `runs`, and leaf-only paths, and
requires the resolved target (including symlinks) to remain under the
resolved manifest directory. It then reads the actual seven referenced
files, verifies every SHA-256, and recomputes `metrics.totalOutputBytes`.
Validation without file I/O is not allowed to claim that a manifest is
valid.

Implement `asset_paths` with this exact body:

```python
def asset_paths(
    map_name: str,
    page_x: int,
    page_y: int,
) -> dict[str, str]:
    normalized = map_name.title()
    stem = f"{normalized}_{page_x:02}_{page_y:02}"
    root = f"/Game/Terrain/Reference/{normalized}"
    return {
        "mesh": f"{root}/SM_{stem}",
        "texture": f"{root}/T_{stem}",
        "material": "/Game/Terrain/Reference/M_TerrainReference",
        "instance": f"{root}/MI_{stem}",
    }
```

Implement the actor helper with this exact body so reports, importer,
checker, and pure tests include the world object between package and
`PersistentLevel`:

```python
def reference_actor_object_path(
    map_package: str,
    map_name: str,
    page_x: int,
    page_y: int,
) -> str:
    world_name = map_package.rsplit("/", 1)[-1]
    normalized = map_name.title()
    return (
        f"{map_package}.{world_name}:PersistentLevel."
        f"ReferenceTerrain_{normalized}_{page_x:02}_{page_y:02}"
    )
```

`validate_manifest` requires schema version 1, algorithm `legacy-fixed-pipeline-v1`, exact page/source-cell/frustum bounds, sorted unique `usedTextureIds`, exact section grid and row-major mask length/content, every canonical manifest-relative file path/hash including paired `.gltf/.bin`, ambient/dwTColor, and all metrics including `totalOutputBytes` and `sharedBoundaryMaxCm`. It recomputes total output bytes from the referenced files and requires equality. It returns structured errors for absent sections, unresolved layers, missing hashes, page-size mismatch, and violated RSS/file/geometry budgets; it must not return an empty list for any invalid manifest.

`overlapping_legacy_tiles` must use strict rectangle overlap on all four edges and return actor names in sorted order.

**Editor entry points and report DTOs:**

```python
# build_reference_terrain_level.py
def run_build(
    manifest_path: Path,
    map_package: str,
    report_path: Path,
) -> dict: ...


# import_reference_terrain.py
def run_import(
    manifest_path: Path,
    map_package: str,
    report_path: Path,
) -> dict: ...


# check_reference_terrain.py
def run_check(
    manifest_path: Path,
    map_package: str,
    level_build_report_path: Path,
    first_import_report_path: Path,
    second_import_report_path: Path,
    report_path: Path,
) -> dict: ...
```

All three scripts parse exactly these positional arguments from
`sys.argv[1:]`,
write their JSON report through `<report>.tmp` plus `os.replace`, and raise
`RuntimeError` after writing the report when any issue exists. A
`Python script executed successfully` line is never treated as success.

`build_reference_terrain_level.py` is the tracked clean-checkout builder for
this terrain-only plan. Its only prerequisites are a successful Task 7
manifest, the tracked `CorsairsUE.uproject`/modules, and the tracked builder
and rule scripts from Task 8. It must not call `place_all_maps.py`: that
existing builder also requires generated scene glTF imports,
`artifacts/maps/garner.objects.json`, `model_map.json`, legacy terrain mesh
imports, materials, lighting, and gameplay placement owned by other plans.
None of those untracked Content products is a hidden prerequisite here.

The builder validates the Task 7 manifest first, requires the literal map
package `/Game/Maps/Garner`, removes any ignored/generated existing map
package, and creates a new level with `LevelEditorSubsystem.new_level`.
Before saving, it resolves
`unreal.load_class(None, REFERENCE_GAME_MODE)`, requires a non-null class,
and sets `world.get_world_settings().default_game_mode` to that exact class.
It also spawns one empty marker actor with object name and label
`ReferenceTerrainBuildRoot` plus tag `CorsairsReferenceTerrainBuildRoot`.
It saves the map, reloads it, and requires
`world.get_outermost().get_name() == "/Game/Maps/Garner"` and
`world.get_path_name() == "/Game/Maps/Garner.Garner"`, then reads the
reloaded `WorldSettings.default_game_mode` and requires its class path to be
exactly `REFERENCE_GAME_MODE`. The resulting terrain-free marker/GameMode
world is only a deterministic bootstrap and is explicitly not a playable or
accepted terrain result. No GREEN or acceptance gate may stop after the
builder; acceptance happens only after the importer has added and validated
the reference terrain actor while preserving the exact GameMode. The base
deliberately contains no scene population or legacy dominant terrain; those
are not fabricated for this test. The importer and checker still enumerate
actual legacy terrain actors and enforce overlap rules, so the same scripts
remain valid when a later full-world builder runs before them.

The builder atomically writes and self-validates this report:

```json
{
  "schemaVersion": 1,
  "kind": "reference-terrain-level-build",
  "manifestPath": "/absolute/repo/artifacts/maps/garner.reference-albedo.json",
  "manifestSha256": "<sha256>",
  "mapPackage": "/Game/Maps/Garner",
  "worldObjectPath": "/Game/Maps/Garner.Garner",
  "markerObjectPath": "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot",
  "defaultGameMode": "/Script/CorsairsGame.CorsairsGameMode",
  "mapFilename": "/absolute/repo/CorsairsUE/Content/Maps/Garner.umap",
  "mapSha256": "<sha256>",
  "replacedExistingMap": false,
  "issues": []
}
```

`validate_level_build_report` requires all fields/types, the literal package,
world/marker paths, literal `defaultGameMode == REFERENCE_GAME_MODE`,
normalized absolute filenames, lowercase 64-hex hashes, and an empty issue
list. `check_reference_terrain.py` validates this report and its SHA-256,
then requires the marker and exact GameMode to exist in the actually loaded
world. The builder report's `mapSha256` is the saved pre-import base hash;
importer mutations are expected to change it.

Every import report has this exact shape; every list and
`finalPackageHashes` is sorted by package/object path:

```json
{
  "schemaVersion": 1,
  "kind": "reference-terrain-import",
  "manifestPath": "<absolute normalized path>",
  "manifestSha256": "<sha256>",
  "mapPackage": "/Game/Maps/Garner",
  "canonicalObjects": {
    "mesh": "/Game/Terrain/Reference/Garner/SM_Garner_17_21",
    "texture": "/Game/Terrain/Reference/Garner/T_Garner_17_21",
    "material": "/Game/Terrain/Reference/M_TerrainReference",
    "instance": "/Game/Terrain/Reference/Garner/MI_Garner_17_21",
    "actor": "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21"
  },
  "created": [
    {
      "objectPath": "<object>",
      "packageName": "<long package name>",
      "beforeSha256": null,
      "afterSha256": "<saved package sha256>"
    }
  ],
  "updated": [
    {
      "objectPath": "<object>",
      "packageName": "<long package name>",
      "beforeSha256": "<sha256>",
      "afterSha256": "<saved package sha256>"
    }
  ],
  "deleted": [
    {
      "objectPath": "<object>",
      "packageName": "<owning long package name>",
      "beforeSha256": "<sha256>",
      "afterSha256": "<owning package sha256 or null when package is gone>"
    }
  ],
  "savedPackages": [
    {
      "packageName": "<long package name>",
      "filename": "<absolute .uasset/.umap path>",
      "sha256": "<sha256>"
    }
  ],
  "finalPackageHashes": [
    {
      "packageName": "<long package name>",
      "filename": "<absolute .uasset/.umap path>",
      "sha256": "<sha256>"
    }
  ],
  "issues": []
}
```

`created`, `updated`, and `deleted` describe object-level mutations and
carry the owning package hash before/after the save. `savedPackages`
contains every package actually written in this run. `finalPackageHashes`
always contains the five managed packages after the run: the four canonical
asset packages and `/Game/Maps/Garner`. Hashes are computed from the saved
`.uasset`/`.umap` files, never from object names or timestamps.

`validate_import_report` rejects missing/wrongly typed fields, unsorted or
duplicate records, malformed hashes, mutation records whose after-hash does
not agree with `finalPackageHashes`, and saved packages absent from the final
set. `validate_idempotent_import_reports` additionally requires identical
manifest/map/canonical-object identity, requires the second report's
`created`, `updated`, `deleted`, and `savedPackages` arrays all to be empty,
and requires byte-for-byte equality of the two `finalPackageHashes` arrays.

The check report has this exact top-level/observation shape. Asset/class
records and package hashes are sorted by path:

```json
{
  "schemaVersion": 1,
  "kind": "reference-terrain-check",
  "manifestPath": "<absolute normalized path>",
  "manifestSha256": "<sha256>",
  "mapPackage": "/Game/Maps/Garner",
  "levelBuildReportSha256": "<sha256>",
  "firstImportReportSha256": "<sha256>",
  "secondImportReportSha256": "<sha256>",
  "actualPackageHashes": [],
  "observations": {
    "loadedWorldPackage": "/Game/Maps/Garner",
    "loadedWorldObject": "/Game/Maps/Garner.Garner",
    "buildMarker": "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot",
    "defaultGameMode": "/Script/CorsairsGame.CorsairsGameMode",
    "assets": [],
    "textureNeverStream": false,
    "material": {
      "blendMode": "masked",
      "shadingModel": "unlit",
      "staticMeshUsage": true,
      "naniteUsage": true,
      "emissiveInput": "T_Garner_17_21.RGB",
      "opacityMaskInput": "T_Garner_17_21.A"
    },
    "instanceParent": "/Game/Terrain/Reference/M_TerrainReference",
    "instanceTexture": "/Game/Terrain/Reference/Garner/T_Garner_17_21",
    "meshMaterial": "/Game/Terrain/Reference/Garner/MI_Garner_17_21",
    "referenceActors": [
      {
        "objectPath": "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21",
        "label": "ReferenceTerrain_Garner_17_21",
        "locationCm": [217600.0, -268800.0, 0.0],
        "boundsCm": [217600.0, -281600.0, 230400.0, -268800.0]
      }
    ],
    "overlappingLegacyActors": [],
    "taggedLegacyActors": [],
    "visibleOverlaps": [],
    "grassOverrides": []
  },
  "issues": []
}
```

Success requires `actualPackageHashes == second.finalPackageHashes`, exactly
one observed build marker, exactly one observed reference actor with the
required world-qualified object path/transform/bounds, actual
`defaultGameMode == REFERENCE_GAME_MODE`, and empty `visibleOverlaps`,
`grassOverrides`, and `issues`.

- [ ] **Step 1: Add RED Python tests**

Build a valid manifest fixture with seven real temporary files and literal
hashes. Deep-copy it for a table-driven mutation test. The table must cover
at least:

```text
schemaVersion missing/wrong
algorithmVersion missing/wrong
page x/y, source bounds, pixelsPerCell, pixel dimensions
ambient and dwTColor
requiredPresentRect
usedTextureIds unsorted/duplicate/out of uint8 range
section origin/dimensions/mask missing, wrong length, or any zero bit
each of the seven file entries missing
leaf-only/absolute/traversing/nested-runs/mixed-run-id/symlink-escape path
wrong manifest-parent base, missing file, malformed hash, file tamper
every metrics member missing or wrong type
RSS/cache/row/PNG/max/RMS/shared-boundary budget violation
absentSectionCount or unresolvedLayerCount nonzero
totalOutputBytes not equal to the seven actual file sizes
```

Every mutation must assert the exact `code` and JSON-pointer-like `field`,
not only that the issue list is nonempty. Add overlap cases for overlap,
one-centimeter separation, and edge-touch on each of the four edges; touching
is not overlap.

Create valid first/second import-report fixtures and mutate each top-level
member and every mutation/hash record. Require rejection when the second
report has one created, updated, deleted, or saved package; when any package
hash differs; when a package is missing/duplicated/reordered; or when
manifest/map/canonical object identity differs. The literal valid pair has
empty second-run mutation arrays and identical five-package hashes.

Create a valid level-build report and delete or alter each required field.
Require exact structured errors for a wrong map package, missing world
object suffix, old actor-style path without `.Garner`, wrong marker path,
missing/wrongly typed/wrong-valued `defaultGameMode`, relative map filename,
malformed map/manifest hash, non-boolean replacement flag, or nonempty
issues. The valid fixture uses only
`/Script/CorsairsGame.CorsairsGameMode`; `/Script/Engine.GameModeBase` and a
Blueprint `_C` path are both rejected with
`{"code":"INVALID_GAME_MODE","field":"/defaultGameMode"}`.

In `test_module_dependencies.py`, add a tracked source-graph
characterization test that reads both Build.cs files, extracts quoted
dependency names, requires `CorsairsImport` in `CorsairsGame` dependencies,
and requires `CorsairsGame` absent from both public and private
`CorsairsImport` dependencies. This standalone test imports no
`reference_terrain_rules` or `unreal` module and must fail if the reverse edge
is introduced; loading the class by soft path is not counted as a module
dependency.

Require exact canonical paths. Add a pure
`assert_legacy_target_allowed(entry_point, asset_path)` helper used by
`setup_terrain_material.py`, `apply_terrain_material.py`, and
`place_terrain.py` immediately before any asset assignment or actor
mutation. For each of those three literal entry-point names, the test
requires `/Game/Terrain/Reference/...` to raise `ValueError` and a normal
`/Game/Terrain/...` path to pass.

Use these exact pure-test names so RED/GREEN output identifies every
contract:

```text
test_asset_paths_are_canonical
test_reference_actor_path_includes_world_object
test_validate_manifest_mutation_matrix
test_validate_level_build_report_mutation_matrix
test_level_build_report_requires_corsairs_game_mode
test_overlap_is_strict_on_all_edges
test_legacy_entry_points_reject_reference_namespace
test_validate_import_report_mutation_matrix
test_second_import_is_zero_mutation_with_identical_hashes
test_idempotence_report_mutation_matrix
```

- [ ] **Step 2: Verify Python RED**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_module_dependencies.ModuleDependencyTests.test_corsairs_import_does_not_depend_on_corsairs_game -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_reference_terrain_rules.ReferenceTerrainRulesTests.test_level_build_report_requires_corsairs_game_mode -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_reference_terrain_rules -v
```

Expected: the dependency-direction characterization passes against the
current Build.cs files; the GameMode-focused command and module command fail
because `reference_terrain_rules.py` does not exist. During implementation,
the GameMode-focused test must remain RED for a missing field,
`/Script/Engine.GameModeBase`, or any value other than the exact Corsairs
class path.

- [ ] **Step 3: Add RED Unreal asset test**

Register two focused automation tests under
`Corsairs.Terrain.ReferenceAssets`:

```text
Corsairs.Terrain.ReferenceAssets.DefaultGameMode
Corsairs.Terrain.ReferenceAssets.ReferenceActor
```

Both use the same explicit canonical-map loader; the first checks the exact
GameMode and the second must load:

```text
/Game/Terrain/Reference/Garner/SM_Garner_17_21
/Game/Terrain/Reference/Garner/T_Garner_17_21
/Game/Terrain/Reference/M_TerrainReference
/Game/Terrain/Reference/Garner/MI_Garner_17_21
```

Before any actor assertion the automation test must explicitly load the canonical map:

```cpp
UWorld* World =
    UEditorLoadingAndSavingUtils::LoadMap(
        TEXT("/Game/Maps/Garner"));
TestNotNull(TEXT("Garner world"), World);
TestEqual(
    TEXT("exact loaded package"),
    World->GetOutermost()->GetName(),
    FString(TEXT("/Game/Maps/Garner")));
TestEqual(
    TEXT("exact loaded world object"),
    World->GetPathName(),
    FString(TEXT("/Game/Maps/Garner.Garner")));
UClass* DefaultGameMode =
    World->GetWorldSettings()->DefaultGameMode.Get();
TestNotNull(TEXT("default game mode"), DefaultGameMode);
const FSoftClassPath ExpectedGameModePath(
    TEXT("/Script/CorsairsGame.CorsairsGameMode"));
UClass* ExpectedGameMode =
    ExpectedGameModePath.TryLoadClass<AGameModeBase>();
TestNotNull(TEXT("soft-loaded expected game mode"), ExpectedGameMode);
TestEqual(
    TEXT("exact default game mode class"),
    DefaultGameMode,
    ExpectedGameMode);
TestEqual(
    TEXT("exact default game mode"),
    DefaultGameMode->GetPathName(),
    FString(TEXT("/Script/CorsairsGame.CorsairsGameMode")));
```

Include `FileHelpers.h`, `GameFramework/GameModeBase.h`,
`GameFramework/WorldSettings.h`, and `UObject/SoftObjectPath.h`. Do not
include a header from the `CorsairsGame` module or name its concrete C++
GameMode type in the `CorsairsImport` module.
`CorsairsImport.Build.cs` currently lists
`UnrealEd` in `PublicDependencyModuleNames`; move that existing dependency to
`PrivateDependencyModuleNames` for the Editor-only module instead of adding a
duplicate. Its public/private dependency arrays must not contain
`CorsairsGame`: `CorsairsGame.Build.cs` already depends on `CorsairsImport`,
so the reverse edge would be a compile-blocking module cycle. The soft class
path and `AGameModeBase` APIs come from the existing `CoreUObject`/`Engine`
dependencies and load the runtime class without a `CorsairsGame` C++
include/link dependency; the pointer comparison and `GetPathName()` assertion
still enforce the exact class. The test may not use the configured startup
map as an implicit substitute.

Require the build marker at
`/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot`, texture
streaming, masked and unlit base material, explicit StaticMesh and Nanite
usage, the MI on both the StaticMesh asset and level component, and exactly
one actor whose full `GetPathName()` is
`/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21`.
Also require no overlapping visible dominant tile and no `MI_grass05`
override.

Require the actor transform and component bounds to reproduce Task 6 exactly: actor `(217600,-268800,0) cm`, world X bounds `[217600,230400]`, and world Y bounds `[-281600,-268800]`, within 1 cm.

- [ ] **Step 4: Verify Unreal RED**

Build succeeds without a `CorsairsImport -> CorsairsGame` dependency, but
automation fails when the clean map/assets/actor or exact Corsairs GameMode
do not exist:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Terrain.ReferenceAssets" \
  -TestExit="Automation Test Queue Empty"
```

- [ ] **Step 5: Implement idempotent headless import**

Implement `build_reference_terrain_level.py` first using the exact clean
builder contract above. Deleting/replacing `/Game/Maps/Garner` is authorized
only inside this builder because `CorsairsUE/Content` is ignored/generated;
the script must reject any map package other than the literal canonical one.
Resolve `REFERENCE_GAME_MODE`, set it through
`world.get_world_settings().set_editor_property("default_game_mode",
game_mode_class)`, and fail rather than substituting `GameModeBase`. After
`save_current_level`, reload and inspect the
package/world/marker/`default_game_mode`, compute the `.umap` hash, validate
the build-report DTO, and only then publish the report. A missing Task 7
manifest, failed manifest validation, failed delete/new/class-load/set/save/
reload, wrong world path, wrong GameMode, or missing/duplicate marker is
fatal.

`import_reference_terrain.py` performs these operations in order:

1. Read and validate the manifest through
   `validate_manifest(data, manifest_path)`. Resolve the glTF, its paired
   `.bin`, and PNG only as `manifest_path.parent / files.*.path` after the
   canonical manifest-relative grammar passes. Abort
   before loading/saving Unreal packages when any hash or budget fails.
2. Require `map_package == "/Game/Maps/Garner"`, load it through
   `LevelEditorSubsystem.load_level`, get the editor world, and require
   `world.get_outermost().get_name() == "/Game/Maps/Garner"`,
   `world.get_path_name() == "/Game/Maps/Garner.Garner"`, and exactly one
   `ReferenceTerrainBuildRoot` marker. It also requires the loaded
   `WorldSettings.default_game_mode` class path to equal
   `REFERENCE_GAME_MODE` and must not modify that property. This makes
   running the tracked builder a checked prerequisite rather than assuming
   an ignored local `.umap`.
3. Snapshot on-disk SHA-256 for the four canonical asset packages and map
   package. Record object existence/type and relevant properties. This
   snapshot is the source of every `beforeSha256`.
4. Reconcile assets by desired state, not by unconditional reimport. Store
   the validated source SHA-256 as asset metadata. Import the PNG/glTF only
   when the canonical asset is absent, has the wrong class, has a different
   source hash, or differs in required import settings. Configure glTF import
   with `AssetImportTask.save=False` and to skip generated
   materials/textures; delete any unexpected sidecar object it nevertheless
   creates, recording it in `deleted`.
5. Build/reconcile `M_TerrainReference` with
   `BLEND_MASKED`, `MSM_UNLIT`, texture RGB connected to emissive, texture A
   connected to opacity mask, and explicit `MATUSAGE_STATIC_MESH` and
   `MATUSAGE_NANITE`. Set `T_Garner_17_21.never_stream=False`. Reconcile the
   MI parent/texture parameter and mesh slot zero. Call setters only when the
   current value differs.
6. Find actors only after the canonical map is loaded. Keep exactly one
   `StaticMeshActor` with object name and label
   `ReferenceTerrain_Garner_17_21`; delete any duplicate reference actor and
   report that object deletion. Set its mesh, component material, transform
   `(217600,-268800,0)` cm, and tag `CorsairsReferenceTerrain`.
7. Compute actual XY bounds for every legacy `Terrain_*` actor from
   `get_actor_bounds(False)`. For strict overlaps with the reference actor
   bounds, persistently hide all StaticMesh components (`visible=False`,
   `hidden_in_game=True`) and add tag
   `CorsairsReferenceTerrainHidden`. Never alter a non-overlapping actor.
   A tagged actor that no longer overlaps is restored and untagged.
8. Track created/updated/deleted objects as each mutation is made. Save only
   dirty changed asset packages with
   `EditorAssetLibrary.save_loaded_asset(..., only_if_is_dirty=True)` and
   save the map only when an actor/component changed. Compute every
   after/final hash only after all saves return success. Write and
   self-validate the import report; any save failure or report issue raises.

`check_reference_terrain.py` is read-only. It first validates the manifest,
the level-build report and its own file hash, both import reports, and the
idempotence relation. It then independently loads `/Game/Maps/Garner`,
requires the exact loaded package `/Game/Maps/Garner`, world object
`/Game/Maps/Garner.Garner`, build marker, and actual
`WorldSettings.default_game_mode == REFERENCE_GAME_MODE`, and inspects
current editor objects rather than trusting any report:

- load the four canonical asset paths and require exact classes;
- inspect actual texture `never_stream`, material blend/shading/StaticMesh
  and Nanite usage, material graph connections, MI parent and texture
  parameter;
- inspect actual mesh slot zero, Nanite setting, the single reference actor,
  component mesh/material, actor transform, and component world bounds;
- require the post-import world to contain both the reference terrain actor
  and exact Corsairs GameMode; the marker-only pre-import bootstrap is never
  reported as playable or accepted;
- enumerate actual legacy `Terrain_*` actors, derive current XY bounds with
  `get_actor_bounds(False)`, and require every strict overlap to carry the
  importer tag and have no visible component;
- require every importer-tagged legacy actor to overlap, proving the importer
  did not hide non-overlapping terrain;
- reject any visible overlap, any `MI_grass05` override, duplicate reference
  actor, or actor/assets from another loaded world;
- hash the five actual saved packages and require exact equality with
  `second.finalPackageHashes`.

The checker writes all observed paths, GameMode, properties, bounds,
overlapping actor names, and actual package hashes to its report. It never
creates, modifies, deletes, marks dirty, or saves an asset or level.

- [ ] **Step 6: Run importer twice and verify GREEN**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_module_dependencies.ModuleDependencyTests.test_corsairs_import_does_not_depend_on_corsairs_game -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_reference_terrain_rules.ReferenceTerrainRulesTests.test_level_build_report_requires_corsairs_game_mode -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_reference_terrain_rules -v
rg -q '"CorsairsImport"' CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs
if rg -q '"CorsairsGame"' \
  CorsairsUE/Source/CorsairsImport/CorsairsImport.Build.cs; then
  echo "forbidden module cycle: CorsairsImport -> CorsairsGame" >&2
  exit 1
fi
mkdir -p artifacts/maps/reports
UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/build_reference_terrain_level.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-level-build.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-import-pass1.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-import-pass2.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/check_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-level-build.json $PWD/artifacts/maps/reports/reference-terrain-import-pass1.json $PWD/artifacts/maps/reports/reference-terrain-import-pass2.json $PWD/artifacts/maps/reports/reference-terrain-check.json" \
  -unattended -nop4 -NullRHI -NoSound
"$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Terrain.ReferenceAssets" \
  -TestExit="Automation Test Queue Empty"
python3 - <<'PY'
import json
from pathlib import Path

root = Path("artifacts/maps/reports")
build = json.loads(
    (root / "reference-terrain-level-build.json").read_text())
first = json.loads(
    (root / "reference-terrain-import-pass1.json").read_text())
second = json.loads(
    (root / "reference-terrain-import-pass2.json").read_text())
check = json.loads(
    (root / "reference-terrain-check.json").read_text())
assert build["mapPackage"] == "/Game/Maps/Garner"
assert build["worldObjectPath"] == "/Game/Maps/Garner.Garner"
assert build["markerObjectPath"] == (
    "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot")
assert build["defaultGameMode"] == (
    "/Script/CorsairsGame.CorsairsGameMode")
assert build["issues"] == []
assert second["created"] == []
assert second["updated"] == []
assert second["deleted"] == []
assert second["savedPackages"] == []
assert second["finalPackageHashes"] == first["finalPackageHashes"]
assert check["actualPackageHashes"] == second["finalPackageHashes"]
assert check["observations"]["loadedWorldObject"] == (
    "/Game/Maps/Garner.Garner")
assert check["observations"]["defaultGameMode"] == (
    "/Script/CorsairsGame.CorsairsGameMode")
assert check["observations"]["referenceActors"] == [{
    "objectPath": (
        "/Game/Maps/Garner.Garner:PersistentLevel."
        "ReferenceTerrain_Garner_17_21"),
    "label": "ReferenceTerrain_Garner_17_21",
    "locationCm": [217600.0, -268800.0, 0.0],
    "boundsCm": [217600.0, -281600.0, 230400.0, -268800.0],
}]
assert check["issues"] == []
PY
```

Expected acceptance: the tracked builder recreates and reloads the canonical
map from a clean ignored `Content` with exact Corsairs GameMode; the builder
alone is not accepted. The standalone dependency test and `rg` gate prove
the existing one-way `CorsairsGame -> CorsairsImport` edge has no reverse
edge. All three subsequent Unreal Python commands exit zero, pass 2 reports
zero mutations and zero saved packages, its five final hashes are identical
to pass 1, and the read-only checker observes the same five hashes, exact
GameMode, and world-qualified reference terrain actor from disk.
`Corsairs.Terrain.ReferenceAssets` passes only after loading that post-import
canonical map.

- [ ] **Step 7: Run full regression and Commit**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests TerrainPageBudgetProbe -j4
ctest --test-dir tools/AssetConverter/build --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s CorsairsUE/Scripts/tests -v
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
git add \
  CorsairsUE/Scripts/reference_terrain_rules.py \
  CorsairsUE/Scripts/build_reference_terrain_level.py \
  CorsairsUE/Scripts/import_reference_terrain.py \
  CorsairsUE/Scripts/check_reference_terrain.py \
  CorsairsUE/Scripts/tests/test_reference_terrain_rules.py \
  CorsairsUE/Scripts/tests/test_module_dependencies.py \
  CorsairsUE/Scripts/setup_terrain_material.py \
  CorsairsUE/Scripts/apply_terrain_material.py \
  CorsairsUE/Scripts/place_terrain.py \
  CorsairsUE/Source/CorsairsImport/Private/Tests/ReferenceTerrainAssetTests.cpp \
  CorsairsUE/Source/CorsairsImport/CorsairsImport.Build.cs
git commit -m "feat(ue): import Garner reference terrain page"
```

## Dependency Order

```text
Task 1 -> Task 2
Task 1 + Task 3 + Task 4 -> Task 5
Task 1 -> Task 6
Task 2 + Task 5 + Task 6 -> Task 7
Task 7 -> Task 8 level builder -> import pass 1 -> import pass 2 -> checker
```

Do not parallelize production edits across these tasks. Converter-only review work may run while Unreal automation is executing, but each task must have a single implementation owner and a clean scoped commit.
