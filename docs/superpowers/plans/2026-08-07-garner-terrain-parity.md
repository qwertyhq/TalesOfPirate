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
- Modify: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ImageCodec.h`
- Modify: `tools/AssetConverter/src/PngWriter.cpp`
- Modify: `tools/AssetConverter/tests/TestImageCodec.cpp`
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

In `TestStreamingPngWriter.cpp`, write red/green and blue/white rows to a 2×2 image, decode it with `DecodeImageFile`, and compare all 16 literal RGBA bytes. Assert `Sha256Bytes` over those raw pixels:

```text
c21b35e3f28e676cedf24c13575a7346682e101a2d26aad9598d0cdbcee9ee3b
```

Require `Sha256File` over a written fixture to equal `Sha256Bytes` over the same file bytes, and require a missing file to return `std::nullopt` with a nonempty `detail`.

Also require:

- two writes of the same rows produce byte-identical PNG files;
- a wrong row length is rejected without consuming a row;
- an extra row is rejected;
- `Finish` before all rows is rejected;
- destroying a writer after an early `Finish` or without `Finish` removes its incomplete run-private file;
- `PeakRgbaRowBytes == 16384` for a 4096-pixel row;
- a low-entropy 4096-pixel fixture produces a PNG smaller than its filtered RGBA input, proving that the old stored-deflate path is not still in use;
- deterministic pseudo-random rows produce more than 64 KiB of compressed payload and at least two consecutive `IDAT` chunks; parse the PNG and require every `IDAT` payload length to be in `1..65536`, then decode and compare the complete RGBA bytes.

In `TestImageCodec.cpp`, replace `Png_WritesImageLargerThanOneDeflateBlock`, whose `size > 65535` assertion characterizes the obsolete uncompressed writer, with a compatibility-adapter test that requires the same large low-entropy image to compress, decode, and match its source pixels. Remove obsolete stored-deflate claims/comments from the adapter path.

- [ ] **Step 2: Verify RED**

Run these commands one at a time; do not overlap them with another build:

```bash
nice -n 10 cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
nice -n 10 ./tools/AssetConverter/build/AssetConverterTests
```

Expected: compilation fails because `StreamingPngWriter.h` and `Sha256.h` do not exist. Record that expected failure before production changes.

- [ ] **Step 3: Implement zlib stream and SHA-256**

Add `find_package(ZLIB REQUIRED)` and link `AssetConverterLib` privately to `ZLIB::ZLIB`. Absence of zlib must stop CMake configuration.

`Open` validates nonzero PNG dimensions and overflow, initializes zlib, and writes the PNG signature and RGBA8 non-interlaced `IHDR`. Use PNG filter byte 0 for every row. Feed that byte and the caller-owned row directly to `deflate(..., Z_NO_FLUSH)`; do not retain a previous row or accumulate the image. Continue each call until all input is consumed, replacing output space and writing an `IDAT` whenever the fixed 64 KiB compressed buffer fills. `Finish` is legal only after exactly `height` rows and repeatedly calls `deflate(..., Z_FINISH)` with fresh output space until `Z_STREAM_END`, writes the final partial `IDAT`, then writes `IEND`. Every chunk CRC covers its type and payload. Call `deflateEnd` exactly once on every initialized stream, including failures.

The writer owns only the current caller row span for the duration of `WriteRgbaRow`, one filter byte, zlib state, and one 64 KiB compressed buffer. `PeakRgbaRowBytes` records RGBA bytes only and may never exceed the largest accepted row. A wrong-length row, extra row, or early `Finish` must not create a successful PNG.

Terrain callers write only to a private `runs/<run-id>/` or test directory. On an unfinished writer or terminal zlib/I/O error, close the stream and remove the incomplete file created by that writer. This task does not publish the terrain artifact set: Task 7 validates every run-private output and is the only owner of the temp-file plus atomic rename of the top-level `garner.reference-albedo.json`. A failed Task 4/5 artifact must never replace that manifest.

Implement `Sha256Bytes` and streaming `Sha256File` in Task 4 so Task 5 can hash the PNG without reading the whole file. Hex output is lowercase and exactly 64 characters. Update `ImageCodec.h` to describe the zlib-backed compatibility adapter instead of claiming that the converter has no external dependency or uses stored deflate. Make existing `WritePng` validate the complete `DecodedImage` and then write its rows through `StreamingPngWriter`.

- [ ] **Step 4: Verify GREEN**

```bash
nice -n 10 cmake -S tools/AssetConverter -B tools/AssetConverter/build \
  -DCMAKE_BUILD_TYPE=Debug
nice -n 10 cmake --build tools/AssetConverter/build --target AssetConverterTests -j4
nice -n 10 ctest --test-dir tools/AssetConverter/build --output-on-failure
```

Expected: all converter tests pass, the compatibility adapter emits readable compressed PNG files, and the multi-`IDAT` test proves the 64 KiB bound.

- [ ] **Step 5: Commit**

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ImageCodec.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/StreamingPngWriter.h \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/Sha256.h \
  tools/AssetConverter/src/StreamingPngWriter.cpp \
  tools/AssetConverter/src/Sha256.cpp \
  tools/AssetConverter/src/PngWriter.cpp \
  tools/AssetConverter/tests/TestImageCodec.cpp \
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

`TerrainPageId` comes from Task 1 `TerrainPage.h`; do not redeclare it in this task. `Sha256Bytes` and `Sha256File` come from Task 4 `Sha256.h`; do not create or duplicate their implementation here.

```cpp
struct LegacyTerrainCornerSample {
    std::array<std::uint8_t, 4> Diffuse;
    double HeightCm;
};

[[nodiscard]] LegacyTerrainCornerSample ResolveLegacyTerrainCornerSample(
    const MapTile& tile, bool present) noexcept;

struct TerrainBakeOptions {
    std::uint32_t CellsPerPage{128};
    std::uint32_t PixelsPerCell{32};
    std::size_t MaxTextureCacheBytes{32u * 1024u * 1024u};
    std::size_t MaxRssBytes{128u * 1024u * 1024u};
    std::size_t MaxPngBytes{96u * 1024u * 1024u};
    std::size_t MaxRgbaRowBytes{16u * 1024u};
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

[[nodiscard]] std::optional<std::size_t> QueryPeakProcessRssBytes(
    std::string& detail);

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

- `ResolveLegacyTerrainCornerSample(tile, true)` decodes `Color` with the exact
  legacy shifts below and signed `Height*10.0` centimetres. With
  `present=false`, two radically different `MapTile` payloads (one all-zero,
  one with every field nonzero/extreme) and controlled mutations of
  `TileInfo`, `BaseTex`, `Color`, `Height`, `Region`, `Island`, and all four
  `Block` bytes all return exactly `Diffuse={255,255,255,255}` and
  `HeightCm=-200.0`. Presence is the only selector; the absent path must not
  inspect any tile field;
- texture UV at source cell 0 equals cell 4;
- legacy `LW_RGB565TODWORD` shift/BGRA semantics are preserved exactly:
  `0xf800 -> {0,0,248,255}`, `0x07e0 -> {0,252,0,255}`,
  `0x001f -> {248,0,0,255}`, and `0xffff -> {248,252,248,255}`;
  do not bit-replicate 5/6-bit channels to 255 and do not reinterpret the
  stored word as conventional red-high RGB565;
- with a full-red base texture, full-blue upper texture, atlas alpha 85,
  `RGB565=0xffff`, ambient `{255,255,255}`, and `dwTColor=0`, source-over first
  gives composite `{170,0,85}`, then legacy tint gives the independently
  hand-derived final pixel `{165,0,83,255}`; this non-half alpha must fail if
  the implementation truncates instead of rounding the blue channel;
- a synthetic red/green/blue/white 2×2 texture page with
  `PixelsPerCell=1`, no upper layers, and `RGB565=0xffff` decodes to literal
  RGBA rows `{248,0,0,255}`, `{0,252,0,255}`, `{0,0,248,255}`,
  `{248,252,248,255}` and raw-pixel SHA-256
  `8bafca593a1e31e011a6efc6afaeb9c010a115a1268abb22cdd4079f1c00244f`;
- corner order is exactly top-left 0, top-right 1, bottom-left 2, bottom-right
  3. The source triangles are `0-1-2` for `localU+localV <= 1` and `3-2-1`
  otherwise. With corner words `{0xf800,0x07e0,0x001f,0x0000}` and a white
  texture, pixels at `(0.25,0.25)` and `(0.75,0.75)` are literally
  `{62,63,124,255}` and `{62,63,0,255}`; bilinear interpolation is a failure;
- an asymmetric texture/alpha fixture produces literal corner/interior pixels that differ under nearest filtering, V flip, WRAP-vs-MIRROR, or missing texel-center offset.

Use this deterministic UNORM rule throughout the baker:

```text
roundUnorm8(x) = clamp(floor(x + 0.5), 0, 255)
upperOver(base, upper, a) =
    roundUnorm8((upper * a + base * (255 - a)) / 255)
final = roundUnorm8(composite * interpolatedLegacyDiffuse / 255)
```

`SampleTerrainImageLinear` has already rounded each sampled RGBA channel by
the Task 3 rule. Blend upper RGB channels in source order and quantize after
every overlay. Obtain every corner's diffuse from
`ResolveLegacyTerrainCornerSample`, barycentrically interpolate each diffuse
RGB channel in double on the selected source triangle, and do not quantize
that interpolation until the final multiply. Final alpha is 255 for a present
textured land cell and 0 for an absent/untextured cell; atlas alpha controls
the RGB overlay and never turns an opaque base cell translucent.

Add dimension and page-math RED cases. `CellsPerPage` and `PixelsPerCell` must
be nonzero. Compute in checked 64-bit arithmetic:

```text
sourceX = page.X * CellsPerPage
sourceY = page.Y * CellsPerPage
pixelWidth = CellsPerPage * PixelsPerCell
pixelHeight = CellsPerPage * PixelsPerCell
rgbaRowBytes = pixelWidth * 4
```

Reject any conversion to `uint32_t`, `size_t`, stream size, or byte count that
would overflow, any zero dimension, and any page whose half-open bounds plus
the required right/bottom one-cell halo exceed the reader's truncated section
grid. The production read is exactly `ReadWindow(SourceCellBounds, 1, 1)`.
Every owned sample (`localX < CellsPerPage && localY < CellsPerPage`) and every
sample in the required-present rectangle must be present. An offset-zero section is allowed
only in the right/bottom halo. The original client returns its shared default
tile for such a neighbor: `dwColor=0xffffffff`, `dwTColor=0`, and
`fHeight=-2.0m`. Task 5 therefore uses literal corner diffuse
`{255,255,255,255}` for an absent halo sample. It must not decode fabricated
RGB565 `0xffff` (`{248,252,248,255}`), and it must not consult zero/stale bytes
in the absent `MapTile` slot.

The halo supplies corner tint only in Task 5. Texture layers and texture/alpha
UVs remain those of the owned cell. Crop the halo out of page section
statistics, the section-presence mask, used texture IDs, absent/unresolved
counts, and catalog resolution. In particular, the runtime default tile's
texture 22 is never sampled or added to `UsedTextureIds` merely because a
halo sample is absent.

- [ ] **Step 2: Add RED real-Garner gates**

Assert:

- all inputs are resolved beneath the explicit repo root as
  `Client/map/garner.map`, `databases/gamedata.sqlite`, client root `Client`,
  and `Client/texture/terrain/alpha/total.png`;
- source cell `(2233,2784)` uses only texture ID 4 and resolves to the literal
  `Client/texture/terrain/brick05.png`;
- page `(17,21)` and required frustum rect `{2193,2756,80,47}` have no absent
  sections;
- result source bounds are exactly `{2176,2688,128,128}`; after excluding the
  right/bottom halo its section result is exactly origin `(272,336)`, grid
  `16x16`, a row-major mask of exactly 256 values, and every value is 1;
- the actual `129x129` Garner page-plus-halo read spans a `17x17` section
  presence mask with exactly one zero at local row-major index
  `14*17+16 == 254`, corresponding to source section `(288,350)` and offset
  table byte `717972`; `TilePresent` has exactly eight zeros at local
  `(x=128,y=112..119)`, source `(2304,2800..2807)`, and every owned sample is
  present;
- those eight absent halo samples contribute literal full-white diffuse in the
  real Garner PNG, while the pure resolver test above—not an impossible attempt
  to mutate value-initialized slots inside a concrete `MapSectionReader`—proves
  stale `MapTile` bytes cannot affect the absent result;
- in `TL,TR,BL,BR` order, the right-edge cells at local `(127,y)` have
  diffuse-presence arrays `[present,default-white,present,default-white]` for
  `y=112..118` and `[present,default-white,present,present]` for `y=119`.
  Every present word is raw `Color=0xffff` and therefore decodes to
  `{248,252,248,255}`, while only the absent entries are literal
  `{255,255,255,255}`;
- no used layer is unresolved;
- `UsedTextureIds` is the sorted unique set of layers actually sampled by page
  cells, excludes halo-only IDs, and is not a hard-coded illustrative set;
- two bakes in distinct private directories produce the same PNG SHA-256;
- each successful path is exactly
  `<outputDirectory>/garner.albedo_<page.X>_<page.Y>.png`, `PngSha256` equals
  `Sha256File(PngPath)`, the hash is 64 lowercase hexadecimal characters, and
  `OutputBytes == std::filesystem::file_size(PngPath)`;
- the in-process unit test requires a 4096×4096 output no larger than 96 MiB,
  `PeakTextureCacheBytes <= 32 MiB`, and `PeakRgbaRowBytes <= 16 KiB`; it does
  not claim an RSS bound because the aggregate `AssetConverterTests` process
  also runs compatibility tests that materialize full Garner.

Add production-path negative tests for every fatal gate: zero/overflow/out-of-
grid page math, missing owned sample/section, missing required-present sample,
missing used catalog ID, unreadable resolved source texture,
unreadable/malformed alpha atlas, RSS limit, PNG-size limit, texture-cache
limit, and RGBA-row limit. Pair the missing-owned mutation with an otherwise
identical missing-halo case which must pass; stale-byte invariance belongs to
the pure resolver test above. Drive each budget comparison through
the same production gate used by `BakeTerrainPage`, not a copied test
predicate. Every fatal case requires `Ok=false`, a nonempty stable `detail`,
no successful hash/path, and no PNG owned by that attempt after return. A used
unresolved ID is always fatal; `UnresolvedLayers` is diagnostic evidence,
never permission to publish.

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

`AssetConverterLib` is the existing shared production library target already used by `AssetConverter` and `AssetConverterTests`; the probe links it instead of compiling a divergent implementation. `TerrainPageBudgetProbe` requires exactly one explicit `--repo-root` argument, rejects missing/extra arguments, resolves every real input beneath that root, performs only one production bake in a fresh process, and exits nonzero unless peak RSS is at most 128 MiB, PNG size at most 96 MiB, texture cache at most 32 MiB, and row staging at most 16 KiB.

Focused in-process tests set `MaxRssBytes=std::numeric_limits<std::size_t>::max()` and assert the recorded value only. The standalone budget probe and production command keep the 128 MiB option and are the only tests that verdict RSS.

`QueryPeakProcessRssBytes` reports the process lifetime peak resident set in
bytes: macOS uses `getrusage(RUSAGE_SELF).ru_maxrss` directly, Linux multiplies
its KiB value by 1024 with checked arithmetic, and Windows uses
`PROCESS_MEMORY_COUNTERS::PeakWorkingSetSize`. An API/OS/overflow failure
returns `nullopt` with nonempty `detail` and makes the bake fail. Do not silently
return zero or current RSS. Query after `Finish`, file size, and SHA work so the
record covers the complete attempt; the fresh-process probe makes the lifetime
peak a meaningful standalone verdict.

- [ ] **Step 3: Verify RED**

Run this build by itself, with no other heavy command active:

```bash
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverterTests TerrainPageBudgetProbe -j4
```

Expected: compilation fails on `TerrainPageBaker.h`. Record that expected failure before production changes.

- [ ] **Step 4: Implement fixed-pipeline bake**

Before rendering, validate every checked dimension and call
`ReadWindow(SourceCellBounds, 1, 1)`. Validate presence before catalog/decode
or output creation: all owned/required-present samples are mandatory, while
right/bottom halo absence is legal and maps to the literal runtime default
corner diffuse. Call `ResolveLegacyTerrainCornerSample(tile, present)` for
each corner and use its `Diffuse`; do not add a second RGB565/default branch.
`TilePresent` is authoritative. Derive the page-only section origin/grid from
the half-open source bounds; do not forward Task 1's halo-inclusive
`SectionPresent` vector as the result mask.

For each output pixel:

1. derive source cell and pixel-center coordinates `localU=(pixelInCellX+0.5)/PixelsPerCell`, `localV=(pixelInCellY+0.5)/PixelsPerCell`;
2. sample base texture with `u=((cellX mod 4)+localU)/4`, `v=((cellY mod 4)+localV)/4`, level-0 linear filtering, top-row V orientation, and WRAP on both axes;
3. for each upper layer, sample its terrain texture with the same coordinates/WRAP; sample the alpha atlas with `u=rectU0+0.01+localU*(0.25-0.02)`, `v=rectV0+0.01+localV*(0.25-0.02)`, linear filtering, and MIRROR on both axes; overlay in source order with the sampled atlas alpha;
4. take each corner diffuse from `ResolveLegacyTerrainCornerSample`, select
   triangle `0-1-2` or `3-2-1`, and barycentrically interpolate its diffuse
   RGB;
5. compute the fixed ambient contribution with ambient bytes `{255,255,255}`
   and `dwTColor={0,0,0}`; never apply the scene-object factor 0.6;
6. apply the specified UNORM quantization order, multiply composite by
   diffuse, and immediately stream that one RGBA row.

The baker may retain only page-plus-halo tiles, one decoded alpha atlas, the
bounded LRU contents, per-pixel/per-layer temporaries, one `pixelWidth*4` RGBA
row, Task 4's 64 KiB compressed buffer, and writer/zlib state. It must never
allocate or retain a full-page RGBA vector, full decoded output, complete PNG,
all source textures outside the cache, or whole-map tiles. The implementation
review records this ownership invariant because `PeakRgbaRowBytes` observes
the writer row, not arbitrary hidden baker allocations.

Resolve and sample layers only for owned page cells. Missing catalog entries
and any source/atlas decode failure are fatal before success. Halo default
texture 22 is not a layer input. Collect the sorted unique page-only IDs,
page-only absent/unresolved counts, cropped section mask, cache and row peaks,
actual file size, actual file SHA, and process peak RSS. Compare every metric
with `<=` against its option, including `MaxRgbaRowBytes`.

Write only the deterministic leaf
`garner.albedo_<page.X>_<page.Y>.png` inside the caller-provided private
run/test directory. Call `StreamingPngWriter::Finish`, then obtain
`OutputBytes` from `filesystem::file_size`, `PngSha256` from Task 4
`Sha256File`, and RSS from `QueryPeakProcessRssBytes`; do not hash decoded
pixels for `PngSha256`. Set `Ok=true` and clear `detail` only after all gates
pass. On validation, catalog, decode, writer, file-size, SHA, RSS, or budget
failure, preserve the first actionable nonempty `detail`, return `Ok=false`,
clean up any partial PNG, and perform checked cleanup of any completed PNG
owned by this attempt. After verified completed-output cleanup, clear successful
path/hash fields. If removal of a completed PNG cannot be verified, return
stable `RECOVERY_REQUIRED`, keep the retained path and available SHA/metrics as
actionable recovery evidence, and never report success. Task 5 never writes or
replaces the top-level manifest; Task 7 alone validates all run-private files
and atomically publishes it.

- [ ] **Step 5: Verify GREEN**

Run every command serially and keep the build at four jobs:

```bash
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests TerrainPageBudgetProbe -j4
nice -n 10 ctest --test-dir tools/AssetConverter/build \
  --output-on-failure -j1
nice -n 10 ctest --test-dir tools/AssetConverter/build \
  --output-on-failure -j1 -R '^TerrainPageBudget$'
nice -n 10 "$PWD/tools/AssetConverter/build/TerrainPageBudgetProbe" \
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

Task 6 is executed in the current serial plan only after Tasks 1–5 are GREEN,
reviewed, and committed. It consumes Task 1 `TerrainPage.h` and the shared
Task 5 `ResolveLegacyTerrainCornerSample` declaration from
`TerrainPageBaker.h`; the GREEN command also builds the Task 5
`TerrainPageBudgetProbe` as a regression gate. Do not copy/redeclare
`MapPageTiles`, `MapCellRect`, `TerrainPageId`, or the corner resolver.

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h`
- Create: `tools/AssetConverter/src/TerrainPageMeshWriter.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainPageMeshWriter.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`

Do not modify `TerrainMeshWriter.*`, `GltfWriter.*`, Tasks 1–5 files, generated
`artifacts`/`Content`, DBs, or pycache. Keep the old
`WriteTerrainMesh(MapTerrain)` compatibility-only and unchanged.

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

- [ ] **Step 1: Add the smallest RED tests**

Use synthetic pages with the production shape: `Cells.Width=128`,
`Cells.Height=128`, `StoredWidth=StoredHeight=129`, and exactly `129*129`
tiles/presence bytes. Synthetic pages below mark every owned sample present.
Add focused tests for:

1. A flat page chooses step 4 and reports literal
   `MaxAbsCm=RmsCm=SharedBoundaryMaxCm=0` with `Samples=16641`.
2. A flat page with one `MapTile::Height=10` (100 cm) at local sample
   `(65,65)` rejects steps 4 and 2 and chooses step 1 with all selected-step
   errors zero. The odd coordinate is intentional: it is not a step-2/4
   vertex.
3. Two horizontal neighboring pages share the same 129 source boundary
   samples: the flat left page chooses step 4, the right page has the interior
   spike and chooses step 1. Decode the emitted glTF buffer data, add each
   result's actor transform, and require the 129 unique shared-edge world
   positions to match exactly and both `SharedBoundaryMaxCm` values to be
   exactly zero. Merely checking two metric fields without inspecting emitted
   vertices is insufficient.
4. Read tracked Garner through `MapSectionReader::ReadWindow(
   {2176,2688,128,128},1,1,...)`, never through `ReadWholeFile`/`ParseMap`.
   Its `17x17` `SectionPresent` has exactly one zero at local row-major index
   `14*17+16 == 254`, source section `(288,350)`, offset-table byte `717972`.
   `TilePresent` has exactly eight zeros at local `(x=128,y=112..119)`, source
   `(2304,2800..2807)`, and no zero in owned `x<128 && y<128` samples.
   Page `(17,21)` must select one of `{4,2,1}` and meet max `<=5` cm, RMS
   `<=2` cm, and shared-boundary error `==0`.
5. Decode a planar output's glTF accessors and paired `.bin` rather than only
   searching JSON text. Require:
   - source/UE-local X is `[0,128]` m and source/UE-local Y is `[0,-128]` m;
   - after the existing generic `WriteGltf` axis conversion, glTF POSITION is
     `(localX, heightMeters, localY)`, so glTF X is `[0,128]` and glTF Z is
     `[-128,0]`; glTF Y is height, not map Y;
   - local `(0,0)` has UV `(0,0)` and `(128,-128)` has UV `(1,1)`;
   - every emitted triangle is non-degenerate and its glTF cross product has
     positive Y, i.e. it is top-facing;
   - decoded index arrays match literal goldens for one interior quad, each of
     the four boundary-strip cases, and each of the four corner fans at steps
     4 and 2; vertex indices refer to the required row-major vertex order;
   - paths are exactly `garner.terrain_17_21.gltf` and
     `garner.terrain_17_21.bin`;
   - actor transform is `(217600,-268800,0)` cm and the local mesh produces
     world X `[217600,230400]`, Y `[-281600,-268800]` cm. Absolute source
     coordinates embedded in vertices are a failure.
6. Decode the real Garner output and require each right-boundary vertex at
   local `(128,-y)`, `y=112..119`, to have glTF height Y exactly `-2.0` m.
   The raw/presence golden is exact: `H(127,112..119)=-60 cm`,
   `H(128,112..119)=-200 cm`, and
   `H(127,120)=H(128,120)=-100 cm`. Thus the owned cells at local `(127,y)`
   have `TL,TR,BL,BR` height arrays `[-60,-200,-60,-200]` cm for
   `y=112..118` and `[-60,-200,-100,-100]` cm for `y=119`. Decode the
   corresponding boundary/corner indices and require those literal heights;
   interpolating through fabricated zero-height tiles is a failure.
   Clone the returned `MapPageTiles`, change all stale `Height`, `Color`,
   `BaseTex`, and `TileInfo` bytes in those eight absent slots, and require
   byte-identical `.gltf` and `.bin`. Unlike the baker's concrete-reader path,
   this in-memory mesh input is directly mutable. Together with the pure Task 5
   resolver test, it proves presence selects the original shared default tile.
7. Two writes into separate temporary directories produce byte-identical
   `.gltf` and `.bin`.
8. Malformed page shape/vector sizes, a zero `TilePresent` sample inside the
   owned `128x128` cells, a `page.Cells` origin inconsistent with `pageId`,
   step 0/unsupported step, non-finite or negative limits, and an unwritable
   final path fail with nonempty `detail`. Also prove that an absent right or
   bottom halo sample is accepted as height `-200` cm regardless of stale bytes
   in its `MapTile` slot. A failure after one member of the pair is written
   removes every regular `.gltf`/`.bin` file owned by that attempt; no stale
   successful pair may remain.

- [ ] **Step 2: Verify RED**

Run by itself at low priority; do not overlap it with another build:

```bash
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverterTests -j4
```

Expected: compile failure because `TerrainPageMeshWriter.h` does not exist.
Record that expected failure before adding production files.

- [ ] **Step 3: Implement adaptive evaluation and mesh output**

Validate before writing:

- `page.Cells == {pageId.X*128, pageId.Y*128, 128, 128}` with checked
  multiplication;
- stored dimensions are exactly `129x129`;
- `Tiles` and `TilePresent` lengths are exactly `16641`, and every owned
  sample with local `x<128 && y<128` is present;
- a missing sample is allowed only on the right/bottom halo
  (`x==128 || y==128`) and is the original runtime default height `-200` cm;
  obtain the height through `ResolveLegacyTerrainCornerSample`;
- every limit is finite and nonnegative.

For every present or absent source sample, use
`ResolveLegacyTerrainCornerSample(tile, present).HeightCm`; do not duplicate
the signed-height conversion or absent default in Task 6. Candidate steps are
tested, inclusively, in the literal order `{4,2,1}`. Preserve all 129 samples
on every outer edge as actual
vertices. Interior vertices for step 4/2 are at step multiples. Step 1 uses
all `129x129` vertices and the literal legacy two-triangle topology for every
cell, making it an exact fallback rather than merely a dense alternate
surface.

The vertex set and index stream are canonical:

1. Include a vertex when it is on the outer ring or both coordinates are
   multiples of `step`. Assign indices by strict row-major source order:
   increasing `(localY, localX)`, before reflecting Y in the stored position.
   Visit the `128/step` by `128/step` coarse cells in the same strict
   `(cellY,cellX)` row-major order when appending triangles.
2. The logical pre-reflection legacy diagonal is
   `TL-TR-BL / BL-TR-BR`. Positions passed to `WriteGltf` already reflect map
   Y as `(x,-y,height)`, so the literal input indices are
   `TL,BL,TR / TR,BL,BR`. Do not pass the pre-reflection order to
   `WriteGltf`; its own axis conversion/winding swap must then produce a glTF
   cross product with positive Y.
3. For a non-corner boundary coarse cell at step 4/2, retain the triangle not
   touching the outer edge and split only the triangle owning that edge into
   unit-edge children. With boundary points `P0..Pstep` ordered by increasing
   X on top/bottom and increasing Y on left/right, emit:

   ```text
   top:   (BL,P{i+1},P{i}); keep (TR,BL,BR)
   bottom:(TR,P{i},P{i+1}); keep (TL,BL,TR)
   left:  (TR,P{i},P{i+1}); keep (TR,BL,BR)
   right: (BL,P{i+1},P{i}); keep (TL,BL,TR)
   ```

   Children are emitted for `i=0..step-1`, followed by the retained triangle.
4. For a corner coarse cell at step 4/2, replace both coarse triangles by one
   fan from the diagonally opposite interior corner. Emit fan triangles
   `(anchor, chain[i], chain[i+1])` in the following literal chain order;
   ranges include every unit boundary point and omit duplicate corner points:

   ```text
   top-left:     anchor BR; chain TR, top right->left through TL,
                                      left top->bottom through BL
   top-right:    anchor BL; chain BR, right bottom->top through TR,
                                      top right->left through TL
   bottom-left:  anchor TR; chain TL, left top->bottom through BL,
                                      bottom left->right through BR
   bottom-right: anchor TL; chain BL, bottom left->right through BR,
                                      right bottom->top through TR
   ```

   No alternative diagonal, fan anchor, vertex duplication, or triangle order
   is allowed. Step 1 never uses these fans; it always uses rule 2.

Build the triangles once per candidate and use those exact triangles for both
evaluation and final output. For every one of the 16641 original grid samples,
evaluate the generated piecewise-linear height in double-precision
centimetres. Both evaluator and final mesh obtain every source height from the
shared resolver; no local raw/default branch is permitted. Set:

```text
MaxAbsCm = max(abs(sourceCm - generatedCm))
RmsCm = sqrt(sum(errorCm * errorCm) / 16641)
SharedBoundaryMaxCm = max abs error over x=0/128 or y=0/128
Samples = 16641
```

An invalid page/step passed to the standalone evaluator returns `Samples=0`
and infinite error fields, never a misleading all-zero success. The writer
chooses the first candidate satisfying all three inclusive option limits.

Emit local source positions `(x, -y, heightCm/100)` metres, normals `+Z`, and
UV `(x/128, y/128)` through the existing `WriteGltf`; its established
source-to-glTF conversion yields `(x,height,-y)`, normals `+Y`, and the paired
`.bin`. Do not duplicate a second glTF serializer. The result paths are:

```text
<outputDirectory>/garner.terrain_<pageX:02>_<pageY:02>.gltf
<outputDirectory>/garner.terrain_<pageX:02>_<pageY:02>.bin
```

Set `ActorWorldXcm=page.Cells.X*100` and
`ActorWorldYcm=-page.Cells.Y*100`. On success clear `detail`. Task 6 writes
only inside the caller's run-private/test directory and never publishes the
top-level manifest; Task 7 owns that atomic publication and recomputes the
glTF/bin hashes from disk. On any mesh/write failure, remove partial regular
files created by this attempt and return `Ok=false`, `Step=0`, empty paths,
and a nonempty `detail`.

- [ ] **Step 4: Verify GREEN**

Run serially at low priority, with at most four build jobs:

```bash
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverterTests TerrainPageBudgetProbe -j4
nice -n 10 ./tools/AssetConverter/build/AssetConverterTests
nice -n 10 ctest --test-dir tools/AssetConverter/build \
  --output-on-failure -j1
```

The first command deliberately retains the Task 5 budget-probe target as a
regression gate; no editor/client process is involved.

- [ ] **Step 5: Commit**

Before staging, require `git diff --check` and inspect the exact path list so
the shared DB/pycache and other agents' work are not included.

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

Task 7 starts only after terrain Tasks 1–6 are GREEN, independently reviewed,
and committed. It consumes their production APIs; it must not copy or
reimplement their map parsing, raster encoding, layer resolution, PNG baking,
SHA-256, process metrics, or page-mesh logic. No Unreal editor/client is used
in this task.

The sole publication boundary is the small top manifest
`artifacts/maps/garner.reference-albedo.json`. All seven large products are
written first into one private `artifacts/maps/runs/<run-id>/` directory. A
failed attempt may leave that directory unreferenced, but may never change the
last valid top manifest or publish a run-internal manifest.

The `terrain-reference` subcommand is dispatched before the legacy directory
converter parses its positional roots or reaches `ReadWholeFile`/
`ParseMap`/`MapTerrain::Tiles`. Its complete inputs are the exact CLI map,
database, client root, alpha atlas, and every catalog-resolved source texture
actually used by the page. The top manifest hashes that complete provenance;
tracked inputs are not trusted merely because they are in git.

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h`
- Create: `tools/AssetConverter/src/TerrainReferenceCommand.cpp`
- Create: `tools/AssetConverter/tests/TestTerrainReferenceCommand.cpp`
- Create: `CorsairsUE/Scripts/install_runtime_map_data.py`
- Create: `CorsairsUE/Scripts/tests/test_install_runtime_map_data.py`
- Modify: `tools/AssetConverter/src/Main.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`
- Modify: `.gitignore`

Do not modify generated `artifacts`, `Content`, `Data/Heights` outputs, DBs,
pycache, Tasks 1–6 production files, Unreal modules, or existing tracked
`.height.r16` files in the commit. Generated outputs are exercised but never
staged.

**Consumed interfaces:**

- Task 1: `MapSectionReader`, `MapPageTiles`, `MapCellRect`, `TerrainPageId`.
- Task 2: `WriteTerrainRasters(reader, basePath, stats, detail)`.
- Task 3: `TerrainCatalog::Load(database, clientRoot, detail)`.
- Task 4: `Sha256File(path, detail)`.
- Task 5: `BakeTerrainPage(reader, catalog, page, alpha, runDirectory,
  bakeOptions, detail)` and `TerrainPageBudgetProbe`.
- Task 6: `WriteTerrainPageMesh(pageTiles, pageId, runDirectory,
  meshOptions, detail)`.

**Produced interfaces:**

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
    INVALID_PROVENANCE,
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
    std::uint64_t SizeBytes;
};

struct TerrainHashedInputDto {
    std::filesystem::path Path;
    std::string Sha256;
};

struct TerrainTextureSourceDto {
    std::uint8_t TextureId;
    std::filesystem::path Path;
    std::string Sha256;
};

struct TerrainManifestSourceDto {
    TerrainHashedInputDto Map;
    TerrainHashedInputDto Database;
    std::filesystem::path ClientRoot;
    TerrainHashedInputDto AlphaAtlas;
    std::vector<TerrainTextureSourceDto> UsedTextures;
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

std::string SerializeTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest);

std::optional<TerrainReferenceManifestDto>
ParseTerrainReferenceManifest(
    std::string_view json,
    std::vector<TerrainManifestIssue>& issues);

std::vector<TerrainManifestIssue>
ValidateTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest,
    const std::filesystem::path& manifestDirectory,
    const TerrainReferenceOptions& limits);

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
    TerrainManifestSourceDto Source;
    TerrainBuildFiles Files;
};

enum class TerrainPublicationStatus : std::uint32_t {
    OK,
    WRITE_FAILED,
    RECOVERY_REQUIRED,
};

struct TerrainPublicationResult {
    TerrainPublicationStatus Status;
    std::string Detail;
    std::filesystem::path RecoveryBackup;
    std::string RecoveryCommand;
};

struct TerrainReferenceDependencies {
    std::function<std::optional<TerrainReferenceBuildProducts>(
        const TerrainReferenceOptions&,
        const std::filesystem::path& runDirectory,
        std::string& detail)> BuildProducts;
    std::function<TerrainPublicationResult(
        const std::filesystem::path& destination,
        std::string_view json)> AtomicPublish;
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

The public serializer is the deterministic DTO-to-JSON seam used by both the
command and tests. The parser is the only JSON-to-DTO seam. It rejects malformed
JSON, duplicate/unknown keys, missing required keys, wrong JSON types,
out-of-range integers, non-finite numbers, and trailing input. A missing member
is exactly `{INVALID_SCHEMA, "<json.pointer>", "required field is missing"}`.
No required value is recovered from a C++ default.

Factor argument parsing into a small testable function in
`TerrainReferenceCommand.cpp` (it may remain private to the translation unit
if tests drive the executable). Tests must prove unknown/duplicate/missing
switches, bad numeric conversions, overflow, zero-size rectangles, and extra
arguments return usage error 2. `--require-present-rect` is always
`X Y Width Height`, never max-X/max-Y.

`Main.cpp` checks the first argument for literal `terrain-reference` before
the legacy `argc < 3`, input/output-root parsing, recursive traversal, or any
call to `ReadWholeFile`. The production test injects guards into the new
dependency path and proves the legacy converter is unreachable for this
subcommand; source inspection alone is not the gate.

**Production dependency call graph:**

1. Before building, recover or fail closed on any unfinished owned top-manifest
   publication journal. Validate options and exclusively create a unique
   regular directory directly under `options.Output/runs/`; reject a reused
   name, symlink/non-directory, and prove its resolved path remains beneath
   resolved `options.Output` before writing.
2. Hash `options.Map`, `options.Database`, and `options.AlphaAtlas` from disk
   with Task 4, record their exact normalized CLI paths plus
   `options.ClientRoot`, and open the map only with Task 1.
3. Call Task 2 with base path `runDirectory / "garner"`, producing exactly
   `garner.height.r16`, `garner.block.raw`, `garner.region.raw`, and
   `garner.terrain.json`.
4. Load the Task 3 catalog and call Task 5 into the same run directory,
   producing exactly `garner.albedo_17_21.png`. For every sorted unique
   `Bake.UsedTextureIds` entry, resolve the exact Task 3 catalog path, require
   it beneath `options.ClientRoot`, record the normalized CLI-root-relative
   mapping `{textureId,path}`, and recompute that source file's SHA-256. The
   mapping vector is strictly increasing by texture ID and its IDs equal
   `Bake.UsedTextureIds` exactly.
5. Read Task 1 window `{2176,2688,128,128}` with right/bottom halo 1 and pass
   that exact page to Task 6, producing both
   `garner.terrain_17_21.gltf` and `garner.terrain_17_21.bin`.
6. Require exactly seven distinct non-symlink regular files, canonical leaf
   names, paths within the one run directory, and no eighth manifest or product.
   Ignore every callback-supplied hash: independently recompute all seven
   SHA-256 values and sizes from disk, then checked-add the sizes into
   `metrics.totalOutputBytes`.
7. Build the DTO from the actual Task 2/5/6 results, not duplicated constants;
   include the complete independently rehashed provenance, serialize,
   parse-roundtrip, compare every DTO field, validate against `options.Output`,
   and only then invoke `AtomicPublish` exactly once.

`Bake.Ok`/`Mesh.Ok`, their output paths, bounds, presence/unresolved counts,
budgets, algorithm, texture IDs, catalog mapping, provenance paths/hashes, and
geometry metrics must agree with the actual inputs and seven disk products. A
successful callback is not trusted merely because it returned `Ok=true` or
supplied a hash string.

There is one path base: the parent of the published top manifest, normally
`artifacts/maps`. Each manifest path is exactly
`runs/<one-run-id>/<expected-leaf>`, uses forward slashes, and resolves as
`manifestDirectory / path`. All seven share one run ID. Reject absolute paths,
empty/dot run IDs, `..`, leaf-only paths, nested/second `runs`, mixed run IDs,
wrong leaves, duplicates, non-regular files, and any symlink escape. Never
validate relative to the run directory.

Source provenance has a separate explicit base: the repository working
directory from which the fixed command is invoked. `source.map.path`,
`source.database.path`, `source.clientRoot`, and `source.alphaAtlas.path` are
the exact normalized forward-slash CLI strings shown below. Each used-texture
path is the normalized `options.ClientRoot / catalogRelativePath` string (for
example `Client/texture/...`), while its file is resolved canonically beneath
`options.ClientRoot`. Reject absolute paths, empty/dot/`..` segments,
backslashes, duplicate/case-alias paths, a client-root mismatch, or a symlink
escape; validation reopens every source through the corresponding already-
validated option rather than joining it to `manifestDirectory`.

The complete schema is the literal schema below: version 1, algorithm
`legacy-fixed-pipeline-v1`, page `(17,21)`, source bounds
`{2176,2688,128,128}`, `pixelsPerCell=32`, dimensions `4096x4096`, ambient
`[1,1,1]`, `dwTColor=0`, required-present rect `{2193,2756,80,47}`, sorted
unique real texture IDs, a `16x16` section mask rooted at `(272,336)` with
exactly 256 ones, complete source provenance, all seven path/hash/size entries,
and every metric from the canonical DTO. `source.usedTextures` is the exact
sorted Task 3 mapping for `usedTextureIds`; its representative single record
below is abbreviated, as are hash strings, `[1]`, every `sizeBytes: 0`, and
observational metric zeros. The real Garner manifest contains every used
texture record, exactly 256 mask ones, seven disk-derived nonzero sizes, their
checked total, measured process/file/geometry metrics, and literal zero
absent/unresolved counts.

```json
{
  "schemaVersion": 1,
  "algorithmVersion": "legacy-fixed-pipeline-v1",
  "source": {
    "map": {"path": "Client/map/garner.map", "sha256": "<sha256>"},
    "database": {"path": "databases/gamedata.sqlite", "sha256": "<sha256>"},
    "clientRoot": "Client",
    "alphaAtlas": {
      "path": "Client/texture/terrain/alpha/total.png",
      "sha256": "<sha256>"
    },
    "usedTextures": [
      {
        "textureId": 4,
        "path": "Client/texture/terrain/brick05.png",
        "sha256": "<sha256>"
      }
    ]
  },
  "page": {
    "x": 17,
    "y": 21,
    "sourceCellBounds": {"x": 2176, "y": 2688, "width": 128, "height": 128},
    "pixelsPerCell": 32,
    "pixelWidth": 4096,
    "pixelHeight": 4096,
    "ambient": [1.0, 1.0, 1.0],
    "dwTColor": 0
  },
  "requiredPresentRect": {"x": 2193, "y": 2756, "width": 80, "height": 47},
  "usedTextureIds": [4],
  "sectionPresence": {
    "originX": 272,
    "originY": 336,
    "width": 16,
    "height": 16,
    "rowMajorMask": [1]
  },
  "files": {
    "height": {"path": "runs/run-001/garner.height.r16", "sha256": "<sha256>", "sizeBytes": 0},
    "block": {"path": "runs/run-001/garner.block.raw", "sha256": "<sha256>", "sizeBytes": 0},
    "region": {"path": "runs/run-001/garner.region.raw", "sha256": "<sha256>", "sizeBytes": 0},
    "terrainMetadata": {"path": "runs/run-001/garner.terrain.json", "sha256": "<sha256>", "sizeBytes": 0},
    "albedo": {"path": "runs/run-001/garner.albedo_17_21.png", "sha256": "<sha256>", "sizeBytes": 0},
    "meshGltf": {"path": "runs/run-001/garner.terrain_17_21.gltf", "sha256": "<sha256>", "sizeBytes": 0},
    "meshBin": {"path": "runs/run-001/garner.terrain_17_21.bin", "sha256": "<sha256>", "sizeBytes": 0}
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

- [ ] **Step 1: Add the smallest RED C++ tests**

Add strict tests for:

- serializer/parser roundtrip and deterministic bytes;
- deletion of every required key, wrong type for every object/array/scalar,
  duplicate/unknown keys, bad integer ranges, malformed/trailing JSON, with
  exact issue code and JSON pointer;
- DTO mutations for schema/algorithm/bounds/texture ordering/mask/file path/
  hash/individual size/total bytes/metric/budget;
- deletion/type/path/hash/tamper mutations for map, database, client root,
  alpha atlas, and each used-texture record; reject unsorted/duplicate/missing
  texture IDs, an ID set different from `usedTextureIds`, a catalog path outside
  the client root, and a same-path file whose bytes changed after the callback;
- path grammar using a manifest under `/tmp/maps` and files under one
  `/tmp/maps/runs/run-001`, including leaf-only, mixed IDs, nested `runs`,
  absolute/traversal paths, wrong leaves, duplicates, extra/eighth products,
  wrong validation base, and symlink escape;
- corrupt map offset, missing used texture, absent required section, PNG/RSS/
  cache/row budget, and max/RMS/shared-boundary violation through the production
  overload, with exit 1 and no changed top manifest;
- the exact CLI argument vector, literal width/height interpretation, and proof
  that `terrain-reference` dispatches before every legacy `ReadWholeFile` path;
- two successful builds under one output root with distinct run IDs; after
  normalizing only `files.*.path` from `runs/<actual-id>/...` to
  `runs/<run-id>/...`, require byte-identical canonical serialization, equal
  seven-file SHA-256/size tuples, and no other semantic difference.

Add these injectable-path tests verbatim:

```text
TerrainReferenceCommand_RejectsInjectedInvalidBakeBeforePublish
TerrainReferenceCommand_RejectsInjectedInvalidMeshBeforePublish
TerrainReferenceCommand_RejectsSerializedRoundTripBeforePublish
TerrainReferenceCommand_PublishesValidatedDtoExactlyOnce
```

Each injected builder creates real temporary files. Invalid cases require
`publishCalls == 0` and a pre-existing manifest byte-identical. The valid case
requires exactly one publish, parses the captured bytes, validates from the top
manifest directory, and proves there is no eighth run-internal manifest.

Add direct production-publisher fault tests with these literal injection
points:

```text
MANIFEST_AFTER_TEMP_FSYNC
MANIFEST_AFTER_BACKUP_FSYNC
MANIFEST_AFTER_PREPARED_JOURNAL_FSYNC
MANIFEST_AFTER_REPLACE
MANIFEST_AFTER_REPLACE_PARENT_FSYNC
MANIFEST_AFTER_REPLACED_JOURNAL_FSYNC
MANIFEST_AFTER_READBACK_VERIFY
MANIFEST_AFTER_COMMITTED_JOURNAL_FSYNC
MANIFEST_ROLLBACK_REPLACE
MANIFEST_ROLLBACK_PARENT_FSYNC
MANIFEST_ROLLBACK_VERIFY
```

Route production publication filesystem primitives through a narrow injected
operations adapter in direct tests; the normal dependency factory binds only
real filesystem calls. The fault seam may not bypass serializer, temp
readback, manifest validation, or final destination verification.

For each pre-commit point test both a prior manifest with non-default mode and
no prior manifest. A one-shot failure/crash must recover exact prior
bytes/mode/existence either in the same call or on a fresh simulated process
startup. Persistent rollback replace/fsync/verification failure is exactly
`RECOVERY_REQUIRED`, retains the immutable verified backup and journal, and
returns their paths plus the literal shell-quoted retry command. It must never
masquerade as ordinary `WRITE_FAILED` or delete its only recovery evidence.
`MANIFEST_AFTER_COMMITTED_JOURNAL_FSYNC` is the post-commit exception: a fresh
startup retains and verifies the complete new manifest and only finishes owned
artifact cleanup; it never rolls a durable `COMMITTED` publication back.

- [ ] **Step 2: Add RED runtime-installer tests**

`install_runtime_map_data.py` accepts exactly two positional arguments:

```bash
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
```

It independently parses the top manifest, enforces the canonical one-run path
grammar, resolves only from `manifest_path.parent`, verifies hashes, and stages
`files.block` plus `files.terrainMetadata` before replacing
`garner.block.raw` and `garner.terrain.json`. Tests reject leaf-only,
wrong-base, mixed-run, traversal, symlink escape, missing/tampered files, wrong
hash, wrong leaf, and partial staging.

The installer is a recoverable two-file transaction, not two best-effort
copies. It acquires the exact exclusive owned lock
`CorsairsUE/Data/Heights/.garner-runtime-install.lock` and, before parsing a
new manifest or creating a stage, resolves any existing same-directory
`.garner-runtime-install.transaction.json`. The 0600 journal is itself updated
by unique same-directory temp, file fsync, `os.replace`, and parent-directory
fsync.

The lock is a no-symlink regular file held with an OS advisory/exclusive file
lock whose ownership is released automatically on process death; stale file
presence alone is not treated as a live owner. If another process still holds
the lock, fail without inspecting, recovering, or deleting its transaction.

The journal records exact normalized target/stage/backup paths, prior
existence/bytes hash/mode, intended source hashes, manifest hash, transaction
ID, and one literal phase:

```text
SNAPSHOT -> PREPARED -> BLOCK_REPLACED -> PAIR_REPLACED -> COMMITTED
```

`SNAPSHOT` is durable before any stage/backup creation or target mutation.
Both source files are copied to unique regular stages in
`CorsairsUE/Data/Heights`, flushed, file-fsynced, closed, reopened, and hashed.
For each existing target, copy exact prior bytes/mode to an immutable unique
same-directory backup, file-fsync and verify it; record absence instead of
inventing a backup for a missing target. Parent-directory fsync makes all
stages/backups durable before `PREPARED` may be journaled.

Replace `garner.block.raw` first with its already-fsynced stage, fsync the
parent, then durably journal `BLOCK_REPLACED`. Replace
`garner.terrain.json` second, fsync the parent, then durably journal
`PAIR_REPLACED`. Reopen both installed targets, require the manifest SHA-256,
exact bytes and intended modes, then durably journal `COMMITTED`. Only a
verified `COMMITTED` pair may remain new; cleanup of journal/backups/stages and
lock is followed by a final parent fsync. A successful return is forbidden
before both targets verify and may never expose a mixed pair as success.

Every journal phase update uses a unique same-directory regular temp, flush,
file fsync, close, strict reparse, `os.replace`, and parent-directory fsync.
An exception after `INSTALL_AFTER_COMMITTED_JOURNAL_FSYNC` retains the complete
verified new pair; a fresh startup verifies that pair and finishes cleanup
rather than rolling a durable commit back. Persistent post-commit cleanup
failure returns `RECOVERY_REQUIRED` with the same literal retry command and
retains the journal plus backups as evidence, but never reports a mixed pair.

On an exception or startup with phase `SNAPSHOT`, `PREPARED`,
`BLOCK_REPLACED`, or `PAIR_REPLACED`, restore the complete old pair. Preserve
each immutable backup by copying it to a separate rollback stage, file-fsync
that stage, atomically replace the target, restore its mode, fsync the parent,
and verify exact prior bytes/mode; remove a target recorded absent, fsync, and
verify absence. A `COMMITTED` startup verifies the complete new pair and only
finishes cleanup. Persistent restore/remove/fsync/verification failure keeps
the journal and every valid backup, exits nonzero with literal
`RECOVERY_REQUIRED`, lists every uncertain path, and prints the exact original
two-argument invocation as the recovery command; rerunning that command always
performs recovery before a new install.

Inject both an ordinary exception and a simulated process death at these exact
boundaries, then create a fresh installer instance and require startup recovery:

```text
INSTALL_AFTER_BLOCK_STAGE_FSYNC
INSTALL_AFTER_METADATA_STAGE_FSYNC
INSTALL_AFTER_BACKUPS_PARENT_FSYNC
INSTALL_AFTER_PREPARED_JOURNAL_FSYNC
INSTALL_BEFORE_BLOCK_REPLACE
INSTALL_AFTER_BLOCK_REPLACE
INSTALL_AFTER_BLOCK_REPLACE_PARENT_FSYNC
INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_FSYNC
INSTALL_BEFORE_METADATA_REPLACE
INSTALL_AFTER_METADATA_REPLACE
INSTALL_AFTER_METADATA_REPLACE_PARENT_FSYNC
INSTALL_AFTER_PAIR_REPLACED_JOURNAL_FSYNC
INSTALL_AFTER_PAIR_VERIFY
INSTALL_AFTER_COMMITTED_JOURNAL_FSYNC
INSTALL_RECOVERY_BLOCK_REPLACE
INSTALL_RECOVERY_METADATA_REPLACE
INSTALL_RECOVERY_PARENT_FSYNC
INSTALL_RECOVERY_VERIFY
```

Python tests inject a narrow file-operations/fault adapter; the two-argument
CLI always binds real `open`/flush/fsync/replace/chmod/remove calls. Injection
cannot replace independent JSON parsing, source/destination hashing, phase
validation, pair verification, or the no-op decision.

A byte-identical second invocation is a true pre-transaction no-op: it briefly
acquires/releases the required exclusive lock but creates no journal/stage/
backup, performs no replace, leaves no lock artifact, and preserves each
target's inode/file identity where available, bytes, mode, size, and mtime.
Expose a pure `compare_deterministic_manifests(first, second)` helper for the
real two-run gate; it independently validates both manifests/files, requires
different run IDs and equal seven hash/size tuples, normalizes only that run-ID
path segment, and compares every remaining DTO field.

The installer intentionally does not overwrite tracked
`CorsairsUE/Data/Heights/garner.height.r16`; `height` and `region` remain
hash-verified run products for scene/future navigation consumers. Add ignored
generated patterns for `*.block.raw`, `*.region.raw`, `*.terrain.json`, and the
exact `.garner-runtime-install.*` transaction artifacts. It never writes a
height/region alias, Unreal content, module file, gameplay configuration, or
runtime-success report.

- [ ] **Step 3: Verify RED**

Run sequentially at low priority, with no other heavy command active:

```bash
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverterTests -j4
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_install_runtime_map_data -v
```

Expected: C++ compile failure on the new command header and Python import
failure because the installer module is absent. Record both RED causes before
production changes.

- [ ] **Step 4: Implement command, strict validation, and atomic publication**

Add this exact invocation:

```bash
nice -n 10 ./tools/AssetConverter/build/AssetConverter terrain-reference \
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

The three-argument `RunTerrainReference` constructs production dependencies
and delegates to the injectable overload. Neither callback may bypass DTO
construction, deterministic serialization, strict parse-roundtrip, disk hash/
size checks, or validation inside `RunTerrainReference`.

Publish only `artifacts/maps/garner.reference-albedo.json`; never rename the
whole output directory, publish a run-internal manifest, or modify/delete an
older run referenced by the current manifest. The production publisher uses an
exclusive owned `artifacts/maps/.garner.reference-albedo.publish.lock` and
recovers an unfinished owned transaction before a new build. Its exact 0600
same-directory journal
`artifacts/maps/.garner.reference-albedo.publish.json` records destination,
unique temp, immutable backup, prior existence/hash/mode, intended manifest
hash and run ID.

This is likewise an OS advisory/exclusive lock on a no-symlink regular file,
released by process death; a live foreign owner fails closed, while a fresh
process may acquire the stale lock file and execute journal recovery.

The journal contains one literal phase:

```text
SNAPSHOT -> PREPARED -> REPLACED -> COMMITTED
```

Every journal phase update is itself a unique same-directory regular temp,
flush, file fsync, close, strict reparse, atomic replacement, and parent-
directory fsync. The lock and every recorded path are required to remain
direct children of `options.Output`; an unexpected symlink or foreign journal
fails closed without deleting it.

Write `SNAPSHOT` durably before creating a manifest temp/backup or mutating the
destination. Write JSON to a unique same-directory regular temp opened
exclusively without following a symlink; flush, file-fsync (`fsync` on POSIX
and the durable Windows equivalent), close, reopen, parse, validate, and hash
the exact temp bytes. If
the destination exists, require a non-symlink regular file, copy its exact
bytes/mode into an immutable unique same-directory backup, file-fsync/reopen/
verify it, and fsync the parent directory. Record prior absence without a fake
backup. Only after temp/backup directory entries are durable may the atomically
updated journal enter `PREPARED`.

Atomically replace the destination from the same filesystem using a primitive
that replaces an existing file on both POSIX and Windows; plain
`std::filesystem::rename` without Windows replacement semantics is not
sufficient. Fsync the parent directory, durably journal `REPLACED`, then reopen
the top manifest, require exact intended bytes/hash and strict parse/
validation. Durably journal `COMMITTED` only after that readback succeeds. A
successful return requires `COMMITTED`, reports the top-manifest SHA-256 and
run ID, removes only owned transaction artifacts, and fsyncs the parent again.

Any pre-commit build/gate/roundtrip/publish failure restores exact prior
bytes/mode/existence. Never consume the immutable backup directly: copy it to a
new rollback temp, file-fsync it, atomically replace the destination, restore
mode, fsync the parent, and verify prior hash/mode; for prior absence remove the
new destination, fsync, and verify absence. A one-shot injected failure returns
`WRITE_FAILED` only after verified rollback. Persistent rollback replace/
remove/fsync/verification failure returns `RECOVERY_REQUIRED`, retains journal
and all valid recovery evidence, and prints the exact shell-quoted full
`terrain-reference` invocation; rerunning it performs recovery before any map,
DB, alpha, catalog, or run-directory work. Startup phases `SNAPSHOT`,
`PREPARED`, and `REPLACED` restore the old state; `COMMITTED` retains and
verifies the new manifest, then finishes cleanup. An unreferenced new run may
remain, but no failed return claims it through a successfully published top
manifest.

If cleanup fails after the `COMMITTED` journal is durable, the complete new
manifest remains authoritative and a fresh startup verifies it before cleanup;
it is never rolled back as though it were pre-commit. Persistent post-commit
cleanup failure is `RECOVERY_REQUIRED` with journal/backup paths and the exact
same full command, never `WRITE_FAILED` and never partial success.

- [ ] **Step 5: Verify GREEN and the real Garner command**

Run one command at a time. Do not start Unreal, Wine/CrossOver, or the original
client during this task:

```bash
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests TerrainPageBudgetProbe -j4
nice -n 10 ./tools/AssetConverter/build/AssetConverterTests
nice -n 10 ctest --test-dir tools/AssetConverter/build \
  --output-on-failure -j1
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_install_runtime_map_data -v
nice -n 10 ./tools/AssetConverter/build/AssetConverter terrain-reference \
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
nice -n 10 cp \
  artifacts/maps/garner.reference-albedo.json \
  artifacts/maps/garner.reference-albedo.run-a.json
nice -n 10 ./tools/AssetConverter/build/AssetConverter terrain-reference \
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
nice -n 10 cp \
  artifacts/maps/garner.reference-albedo.json \
  artifacts/maps/garner.reference-albedo.run-b.json
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -c \
  'import sys; from pathlib import Path; from CorsairsUE.Scripts.install_runtime_map_data import compare_deterministic_manifests; issues = compare_deterministic_manifests(Path(sys.argv[1]), Path(sys.argv[2])); print("\n".join(issues)); raise SystemExit(1 if issues else 0)' \
  artifacts/maps/garner.reference-albedo.run-a.json \
  artifacts/maps/garner.reference-albedo.run-b.json
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
nice -n 10 rm -f \
  artifacts/maps/garner.reference-albedo.run-a.json \
  artifacts/maps/garner.reference-albedo.run-b.json
nice -n 10 ps -axo pid,etime,%cpu,%mem,nice,command | \
  nice -n 10 rg -i 'UnrealEditor|Game\.exe|CrossOver|wine|Build\.sh' || true
nice -n 10 pmset -g therm
```

GREEN evidence must name both distinct run IDs; two equal ordered sets of seven
independently verified hashes/sizes; equal semantic manifests after only
run-prefix normalization; complete equal input-provenance hashes/mappings; the
exact published manifest path/hash; zero absent/unresolved counts; budget/
geometry metrics; the two installed runtime files; and literal `NOOP` from the
second installer invocation with unchanged file identity/bytes/mode/size/mtime.
Generated outputs and recovery artifacts stay ignored and no transaction
journal, lock, temp, or backup remains after success.

- [ ] **Step 6: Commit only tracked Task 7 sources**

Run `git diff --check`, inspect the exact path list, and exclude shared DB,
pycache, artifacts, Content, installed runtime data, and other agents' work.

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

**Verified downstream boundary (not Task 7 work):**

- Terrain Task 8 consumes the top manifest plus `albedo`, `meshGltf`, and
  `meshBin`; its tracked clean-checkout builder owns `/Game/Maps/Garner`, the
  exact `/Script/CorsairsGame.CorsairsGameMode`, material/assets, two-pass
  idempotence, and the read-only checker. Task 7 must not depend on ignored
  `Content` or attempt any UE import.
- Movement Task 6 consumes the installed `garner.block.raw` and
  `garner.terrain.json`, removes the runtime `CorsairsGame -> CorsairsImport`
  editor-module dependency, and proves the Mac Game target. Task 7 changes no
  Build.cs and makes no cook/runtime-success claim.
- The exact 1920×1080 tick-120 original/UE visual capture, camera contract,
  side-by-side image, and client cleanup belong to scene parity Task 8. Task 7
  supplies its hash-linked terrain manifest only; it launches neither client.

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
