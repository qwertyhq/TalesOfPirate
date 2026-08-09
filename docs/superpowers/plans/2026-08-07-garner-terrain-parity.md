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
    // Test-only completed-file remover; empty in production. Its return is advisory.
    std::function<bool(const std::filesystem::path&, std::string&)>
        TestOnlyRemoveCompletedOutput;
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

`TerrainBakeOptions::TestOnlyRemoveCompletedOutput` is the only public
test seam for completed-file removal. Production callers must leave it empty.
The callback return is advisory: after either the injected or real remove,
physical state from `symlink_status` is authoritative.

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

`AssetConverterLib` is the existing shared production library target already used by `AssetConverter` and `AssetConverterTests`; the probe links it instead of compiling a divergent implementation. `TerrainPageBudgetProbe` requires exactly one explicit `--repo-root` argument, rejects missing/extra arguments, resolves every real input beneath that root, atomically creates a unique output directory which it alone owns, performs only one production bake in a fresh process, and exits nonzero unless peak RSS is at most 128 MiB, PNG size at most 96 MiB, texture cache at most 32 MiB, and row staging at most 16 KiB. Probe cleanup uses the same physical `symlink_status` postcondition and fails if any directory or dangling symlink remains.

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
actionable recovery evidence, and never report success. Normalize only the
expected `no_such_file_or_directory` status; cleanup is verified exactly when
`symlink_status` has no error and reports `file_type::not_found`. A dangling
symlink is retained state and therefore requires recovery. Task 5 never writes
or replaces the top-level manifest; Task 7 alone validates all run-private
files and atomically publishes it.

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
    // Test-only paired writer; empty in production. Its status is advisory:
    // physical pair validation and cleanup remain authoritative.
    std::function<GltfStatus(
        const LgoGeomObj&, const std::filesystem::path&, std::string&)>
        TestOnlyWriteGltf;
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

`TerrainPageMeshOptions::TestOnlyWriteGltf` is the only public Task 6 seam
for deterministic paired-writer failures. Production callers, including Task
7, must leave it empty. When present, Task 6 calls it exactly once instead of
the real `WriteGltf`, after both physical output leaves were confirmed missing
and `writeAttemptStarted` was set. It receives the completed `LgoGeomObj`, the
final `.gltf` path, and `detail`, and may create a partial physical pair before
returning a `GltfStatus`. The returned status is advisory: the normal failure
cleanup and successful-pair validation use physical `symlink_status` state as
authority.

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
   successful pair may remain. Exercise that cleanup deterministically through
   `TestOnlyWriteGltf`: write a regular `.bin`, return `WRITE_FAILED` before a
   `.gltf` exists, require one callback invocation, an empty failure result,
   actionable `detail`, and both physical leaves absent afterward.

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
`.bin`. Do not duplicate a second glTF serializer. An empty
`TestOnlyWriteGltf` always selects this production path; a nonempty callback is
test-only and occupies the same single call site after write ownership begins.
The result paths are:

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
   `metrics.totalOutputBytes`. Before any manifest publication, pass all seven
   closed outputs to `DurabilizeRunProducts(durable_fs, ...)`, a command helper
   composed only from the adapter operations below: fsync or
   `FlushFileBuffers` every file, make the run-directory entries durable by the
   platform contract, reopen and independently recompute every hash/size again,
   and reject any changed/missing/eighth product.
7. Build the DTO from the actual Task 2/5/6 results, not duplicated constants;
   include the complete independently rehashed provenance, serialize,
   parse-roundtrip, compare every DTO field, validate against `options.Output`,
   and only then invoke `AtomicPublish` exactly once.

`durable_fs` is one cross-language behavioral contract, implemented as a narrow
C++ adapter inside the command publisher and a narrow Python adapter inside the
installer. Both expose the same audited operations: `OpenExclusiveTemp`,
`FlushFile`, `ReserveMoveTarget`, `ReplaceSameVolume`,
`SyncDirectoryOrEquivalent`, `RemoveOwned`, `RestoreMode`, `LockExclusive`, and
the separate `RetireLock`. Every mutating, durability, mode-restoration, and
locking primitive in top-manifest publication, installer publication, journal
phases, rollback, recovery, and owned-artifact cleanup goes through the adapter;
independent read/stat/hash/parse verification remains outside it. No production
code outside that adapter may call raw `rename`/`os.replace`, `fsync`,
`chmod`/mode mutation, delete, or a private platform shortcut around the
injected seam.

On POSIX, `OpenExclusiveTemp` uses a unique same-directory path with
`O_CREAT|O_EXCL|O_NOFOLLOW`, mode 0600; `FlushFile` flushes any open language
stream or reopens a closed owned output without following links, then performs
`fsync(fd)` on an `O_RDWR` regular-file descriptor; `ReplaceSameVolume` first
proves equal `st_dev` and then uses
atomic `rename`/`os.replace`; and
`SyncDirectoryOrEquivalent` opens the parent with `O_DIRECTORY|O_RDONLY` and
`fsync`s it. `RestoreMode` is `fchmod` followed by `FlushFile`, and
`RemoveOwned` is `unlink` followed by that same parent barrier.
The real POSIX gate executes these operations on a temporary filesystem,
including replace, rollback, directory fsync, and injected failure recovery.

On Windows, the adapters use documented direct Win32 calls (Python through
`ctypes.WinDLL(..., use_last_error=True)` with explicit `argtypes`/`restype`,
C++ through the same APIs with immediate `GetLastError` capture):
`CreateFileW(..., CREATE_NEW, ...)` for a same-directory exclusive regular temp,
`FILE_FLAG_OPEN_REPARSE_POINT` plus `GetFileInformationByHandleEx` for
reparse-point rejection, `FlushFileBuffers` on every writable file handle, and
`MoveFileExW(source, destination,
MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)` (`0x1|0x8`) for replacement.
The adapter compares roots/serials with
`GetVolumePathNameW`/`GetVolumeInformationW` before replacement, never sets
`MOVEFILE_COPY_ALLOWED`, and never falls back to copy/delete. For the seven
already-named private run outputs, `SyncDirectoryOrEquivalent(parent,
entriesToFinalize)` first flushes each file, then uses the typed reservation
contract below and moves each canonical file through that same-directory name
and back with write-through `MoveFileExW`; stages/backups use the same entry-
finalization mode. After `ReplaceSameVolume`, an empty entry set records
that operation's write-through move as the equivalent barrier. Only after all
canonical output names are restored and rehashed may the top manifest commit
begin. A failure leaves only an unreferenced private run and cannot change the
prior top manifest.

Every Windows placeholder is created only by
`ReserveMoveTarget(source, reserved, kind, transactionId, sourceHash)`. It uses
`CREATE_NEW`, writes an exact versioned reservation record containing those
fields plus both normalized paths, calls `FlushFileBuffers`, reopens without
following reparse points, verifies the same file identity and byte-exact record,
then closes it before `MoveFileExW`. All transaction reservation paths/records
are fixed in `SNAPSHOT`; run-output reservations are derived only from the
private run ID and canonical leaf. A crash before the move is therefore not an
ambiguous empty file.

Startup/current-call recovery applies one exact state table. Canonical source
present plus the exact reservation record means pre-move: remove only that
reservation and retry. Canonical source absent plus a non-reservation tombstone
whose bytes/hash/identity match the recorded intended source means post-move:
resume its documented cleanup/replacement. Canonical absent plus a reservation,
canonical present plus a mismatched payload, an invalid record, or any
unrecorded path is foreign/corrupt and fails closed. No raw existence heuristic
or directory order chooses a state.

Windows `RemoveOwned(path, tombstone)` never invents a path at deletion time.
Before `SNAPSHOT`, the transaction derives each ordinary artifact tombstone
from its transaction ID and durably records the normalized direct-child mapping
in its journal; each lock and canonical journal instead has the one fixed
retired path named literally in its flow below. `RemoveOwned`
requires that mapping, completes `ReserveMoveTarget` with kind `remove-owned`,
then moves the validated canonical path over the closed reservation with
`MoveFileExW(...,
MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)` and verifies the canonical
path is absent. A pre-existing path is handled only by the exact state table;
an unrecorded/foreign tombstone is never overwritten. For a closed ordinary
artifact, read its tombstone
attributes, clear only `FILE_ATTRIBUTE_READONLY` through audited
`SetFileAttributesW` (use `FILE_ATTRIBUTE_NORMAL` only if no bit remains),
capture/readback-verify the result, then call `DeleteFileW` and verify exact
absence. This cleanup is replayable and successful return requires absence. The
canonical journal is removed last. Startup under the lock checks both its exact
canonical path and its one recorded/deterministic journal-tombstone path,
then applies the exact state table: canonical plus its exact reservation is
recoverable pre-move, while canonical absent plus its matching journal payload
is post-move recovery. Two journal payloads, a mismatch, or any unrecorded/
foreign candidate fails closed. Thus a crash after journal-to-tombstone remains
discoverable.

`RemoveOwned` may retire journal/backup evidence only after the committed new
state or exact old state independently verifies. A crash may then leave only
recorded tombstones, never resurrect a canonical target or erase evidence still
required for recovery. `RestoreMode` is valid only on an unpublished owned
temp/stage: Windows calls `SetFileAttributesW`, immediately captures its error,
readback-verifies the recorded observable attributes, and then calls
`FlushFileBuffers`; POSIX applies `fchmod` before the same final `FlushFile`.
Only after that does `ReplaceSameVolume` publish the staged file. No critical
path mutates canonical mode/attributes after replacement.

POSIX and Windows locks use `fcntl`/`flock` and `LockFileEx` respectively, so
kernel ownership is released on process death. `LockExclusive(canonical,
fixedRetired)` opens/creates a no-link regular file, acquires the kernel lock,
and establishes the exact versioned owner/canonical-path marker before any
transaction work. For a new file it writes the marker, calls `FlushFile`
(`FlushFileBuffers` on Windows), reopens without following links, requires the
same file identity, and readback-verifies exact bytes. For an existing file it
requires the same exact marker bytes. Only then compare the locked handle
identity with a fresh no-follow lookup of the canonical path: POSIX compares
device/inode from `fstat`/`lstat`; Windows compares volume serial/file ID from
handle queries and opens the lock with delete sharing. Missing or mismatched
identity releases/closes and retries the current path before retired-lock/
journal inspection or mutation; a matching handle held by another process
fails closed. Canonical lock-directory durability is not a safety precondition,
but marker file-data durability/readback is mandatory for later retired-lock
recovery.

After identity matches and before journal inspection, validate and remove only
the literal `fixedRetired` path left by an earlier crash. It must be a direct
no-link regular file with either the exact typed `retire-lock` reservation or
the exact lock marker. The reservation additionally records the locked handle's
volume serial/file ID and marker hash: matching current identity means
pre-move, so delete the reservation and retry retirement later; an exact old
lock marker with a different file ID means post-move, so clean that retired
lock. Any other combination is foreign and fails closed. On Windows, delete-
pending/busy means close the new lock without transaction work and return
retryable non-success. No glob chooses recovery state.

`RetireLock` is distinct from `RemoveOwned`. It runs only after all shared-state
cleanup/barriers are complete and while the exact handle remains locked. After
one last handle-to-path identity check, POSIX moves canonical to the fixed
retired path, unlinks/fsyncs that name, then closes the handle as its final
operation. Windows completes `ReserveMoveTarget` with kind `retire-lock`, the
lock generation ID, current file ID, and exact marker hash, then moves canonical
over the closed reservation with same-volume `MoveFileExW(...,
MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`, verifies canonical absence
and handle identity, and requires `DeleteFileW(fixedRetired)` to accept
delete-pending. It does not require retired-path absence while the handle is
open; `CloseHandle` is the final call and documented delete-on-close completes
removal. A crash before acceptance is recovered from the exact retired path on
the next acquisition. No filesystem call follows the final close. Every waiter
repeats the post-acquire identity check, so an opener of the old inode/handle
can never enter concurrently with an owner of a newly created lock path. A
retirement failure is non-success, never a successful return with a lock
artifact.

Windows does not attempt unsupported directory-handle `fsync`,
`FlushFileBuffers` on a directory, or an administrator-only volume flush.
There, each write-through `MoveFileExW` is the directory-entry durability
barrier; journal phase changes and recovery use that same primitive. The
Windows gate statically verifies the exact `ctypes` signatures/flags and runs a
behavioral mocked adapter that checks call order, `FlushFileBuffers` for every
file, same-volume rejection, injected failures, and startup recovery. No mocked
gate may replace the real POSIX gate. Forced POSIX `EIO`, Windows error 5, and
the contract cross-volume case assert the exact error prefixes below; tests
also prove the Win32 code is captured before any formatting/cleanup API call.

Run the literal cross-platform race fixture
`DurableFsLock_RejectsStaleHandleAfterRetirement` for both production locks: a
waiter opens the old lock identity, the owner retires the path as its last
filesystem action, and a new owner creates the canonical path. The stale waiter
must fail the post-acquire identity check and may never enter the critical
section. `DurableFsLock_DurableMarkerBeforeRecovery` injects crash before/after
marker flush/readback and forbids journal work until exact recovery bytes are
durable. Windows static/mocked tests
`DurableFsWindows_RemoveOwnedRecoversJournalTombstone` and
`DurableFsWindows_StagesAttributesBeforeReplace` require exclusive tombstone
reservation, exact startup discovery after journal retirement, no foreign
overwrite, READONLY tombstone cleanup without losing other attributes,
mode/attribute staging before flush+replace, and no post-replace canonical
attribute mutation. `DurableFsWindows_RecoversTypedMoveReservation` injects a
crash after reservation flush/readback but before `MoveFileExW` for an ordinary
tombstone, journal retirement, run-entry finalization, and lock retirement; it
requires the exact pre/post-move state table and rejects every mismatched
record/hash/path/file ID. `DurableFsWindows_RetireLockUsesDeleteOnClose` mocks
crash before/after move and delete-pending acceptance, forbids retired-path stat after
acceptance, requires `CloseHandle` last, and proves the next exact-path
acquisition recovers a pre-acceptance retired lock.

All adapter failures have one exact single-line machine-testable prefix before
the optional native message:
`DURABLE_FS_ERROR op=<operation> path=<JSON-quoted-normalized-path> native=errno:<decimal>:`
on POSIX and the same prefix with
`native=win32:<GetLastError-decimal>:` on Windows. Two-path operations replace
`path` with `source=<JSON-quoted-normalized-path>
destination=<JSON-quoted-normalized-path>` on that same line. A cross-volume
rejection ends the prefix with `native=contract:CROSS_VOLUME:`. The operation,
normalized path(s), and native code are preserved unchanged in
`WRITE_FAILED`/`RECOVERY_REQUIRED` detail.

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
observational metric zeros. In particular, the example's `peakRssBytes: 0` is
an illustrative placeholder and is invalid in a production manifest. The real
Garner manifest contains every used texture record, exactly 256 mask ones,
seven disk-derived nonzero sizes, their checked total, measured process/file/
geometry metrics, and literal zero absent/unresolved counts. `peakRssBytes` is
the actual OS process-lifetime measurement from Task 5 and independently
validates as `0 < peakRssBytes <= options.Bake.MaxRssBytes` (128 MiB for the
fixed command); it is never replaced by a deterministic constant.

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
- two successful builds under one output root with distinct run IDs; validate
  each actual manifest independently, including its actual nonzero bounded
  `metrics.peakRssBytes`, and compare the seven products byte-for-byte as well
  as by equal ordered SHA-256/size tuples. The deterministic projection
  normalizes only each `files.*.path` run segment from `<actual-id>` to
  `<run-id>` and, only after both actual values validate, replaces only
  `metrics.peakRssBytes` with projection-only `std::uint64_t{0}`; its canonical
  serialization must be byte-identical and every other DTO field must compare
  exactly. The actual manifests are not required or claimed to be
  byte-identical.

Add literal RED fixtures
`TerrainDeterministicProjection_AllowsDifferentValidPeakRss`,
`TerrainManifest_RejectsZeroPeakRss`,
`TerrainManifest_RejectsPeakRssOverBudget`, and
`TerrainDeterministicProjection_RejectsAnyOtherFieldDifference`. The first
uses two different values both in `(0, 128 MiB]` and must pass; the next two
must fail independent validation; the last mutates each non-RSS DTO field in
turn and must fail comparison.

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
MANIFEST_AFTER_TEMP_FLUSH
MANIFEST_AFTER_BACKUP_FLUSH
MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE
MANIFEST_AFTER_REPLACE
MANIFEST_AFTER_REPLACE_DURABILITY_BARRIER
MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE
MANIFEST_AFTER_READBACK_VERIFY
MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE
MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE
MANIFEST_AFTER_JOURNAL_RETIRE
MANIFEST_ROLLBACK_REPLACE
MANIFEST_ROLLBACK_AFTER_MODE_STAGE_FLUSH
MANIFEST_ROLLBACK_DURABILITY_BARRIER
MANIFEST_ROLLBACK_VERIFY
MANIFEST_AFTER_LOCK_RESERVATION_DURABLE
MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED
MANIFEST_AFTER_LOCK_DELETE_PENDING
```

Route production publication through the injected `durable_fs` operations
adapter in direct tests; inject before/after the named adapter operation, not
at raw POSIX calls. The normal dependency factory binds only the real platform
adapter. The fault seam may not bypass serializer, temp readback, manifest
validation, run-product durability, or final destination verification.

Add failure iteration over each of the seven `FlushFile` operations and each
run-entry `SyncDirectoryOrEquivalent` sub-operation. Every such failure must
leave the prior top manifest byte-identical and the new run unreferenced, with
`AtomicPublish` uncalled. The POSIX real-filesystem gate and Windows static/
mocked-behavior gate above are mandatory in addition to the platform-neutral
fault matrix.

For each pre-commit point test both a prior manifest with non-default mode and
no prior manifest. A one-shot failure/crash must recover exact prior
bytes/mode/existence either in the same call or on a fresh simulated process
startup. Persistent rollback replace/barrier/verification failure is exactly
`RECOVERY_REQUIRED`, retains the immutable verified backup and journal, and
returns their paths plus the literal shell-quoted retry command. It must never
masquerade as ordinary `WRITE_FAILED` or delete its only recovery evidence.
`MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE` is the post-commit exception: a fresh
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
`CorsairsUE/Data/Heights/.garner-runtime-install.lock`, whose fixed retired path
is `.garner-runtime-install.lock.retired`. After the mandatory post-acquire
identity check and before parsing a new manifest or creating a stage, it
applies the typed-reservation state table to
`.garner-runtime-install.transaction.json` and its one exact retired path
`.garner-runtime-install.transaction.json.retired`. Canonical journal plus its
exact retirement reservation is pre-move cleanup; canonical absent plus the
retired payload matching the recorded journal hash is post-move recovery.
Two journal payloads or any other combination fails closed. After strict parse, any
`.garner-runtime-install*.retired` path not equal to the fixed lock/journal path
or an exact journal-recorded tombstone fails closed. The 0600 journal is itself
updated only through `durable_fs`: unique same-directory `OpenExclusiveTemp`,
flush plus `FlushFile`, strict reparse, `ReplaceSameVolume`, and
`SyncDirectoryOrEquivalent`.

The lock follows the exact handle-to-path identity/retry and last-filesystem-
action retirement protocol above. Stale file presence alone is not treated as
a live owner. If another process still holds the matching lock identity, fail
without inspecting, recovering, or deleting its transaction.

The journal records exact normalized target/stage/backup paths, every typed
reservation and rollback/cleanup tombstone mapping (including its own fixed
retired path), prior
existence/bytes hash/mode, intended source hashes/modes, manifest hash,
transaction ID, and one literal phase:

```text
SNAPSHOT -> PREPARED -> BLOCK_REPLACED -> PAIR_REPLACED -> COMMITTED
```

`SNAPSHOT` is durable before any stage/backup creation or target mutation.
Both source files are copied to unique regular stages in
`CorsairsUE/Data/Heights`; apply each intended mode/attributes to the unpublished
stage through `RestoreMode`, flush through `FlushFile`, close, reopen, and hash/
mode-verify it.
For each existing target, copy exact prior bytes/mode to an immutable unique
same-directory backup, apply the recorded prior mode while it is unpublished,
`FlushFile` and verify bytes/mode; record absence instead of
inventing a backup for a missing target. `SyncDirectoryOrEquivalent` makes all
stage/backup entries durable before `PREPARED` may be journaled: POSIX fsyncs
the directory, while Windows performs the documented same-directory
write-through finalization and never attempts directory or volume flush.

Replace `garner.block.raw` first with its already-flushed stage through
`ReplaceSameVolume`, complete `SyncDirectoryOrEquivalent`, then durably journal
`BLOCK_REPLACED`. Replace `garner.terrain.json` second through the same two
adapter operations, then durably journal `PAIR_REPLACED`. On POSIX the barrier
is parent-directory fsync; on Windows it is the completed write-through
`MoveFileExW` and never a directory flush. Reopen both installed targets,
require the manifest SHA-256, exact bytes and intended modes, then durably
journal `COMMITTED`. Only a verified `COMMITTED` pair may remain new; cleanup
uses only journal-recorded `RemoveOwned` tombstones, retires the journal last,
and completes a final `SyncDirectoryOrEquivalent`. `RetireLock` is the last
filesystem action. A successful return is forbidden before both targets verify
and may never expose a mixed pair as success.

Every journal phase update uses `OpenExclusiveTemp`, flush plus `FlushFile`,
close, strict reparse, `ReplaceSameVolume`, and
`SyncDirectoryOrEquivalent`; no installer mutation path calls raw
`os.replace`/`fsync`/`chmod`/remove outside the adapter.
An exception after `INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE` retains the
complete verified new pair; a fresh startup verifies that pair and finishes
cleanup rather than rolling a durable commit back. Persistent post-commit
cleanup failure returns `RECOVERY_REQUIRED` with the same literal retry command
and retains the journal plus backups as evidence, but never reports a mixed
pair.

On an exception or startup with phase `SNAPSHOT`, `PREPARED`,
`BLOCK_REPLACED`, or `PAIR_REPLACED`, restore the complete old pair. Preserve
each immutable backup by copying it to a separate rollback stage, applying
its recorded prior mode/attributes to that unpublished stage through
`RestoreMode`, then `FlushFile` and atomically `ReplaceSameVolume` the target.
Complete `SyncDirectoryOrEquivalent` and verify exact prior bytes/mode; remove a
target recorded absent only through its journal-recorded `RemoveOwned`
tombstone, complete the same barrier, and verify absence. A `COMMITTED` startup
verifies the complete new pair and only finishes cleanup. A startup that finds
only the retired journal performs the same target verification/recovery before
replaying tombstone cleanup. Persistent restore/remove/barrier/verification
failure keeps
the journal and every valid backup, exits nonzero with literal
`RECOVERY_REQUIRED`, lists every uncertain path, and prints the exact original
two-argument invocation as the recovery command; rerunning that command always
performs recovery before a new install.

Inject both an ordinary exception and a simulated process death at these exact
boundaries, then create a fresh installer instance and require startup recovery:

```text
INSTALL_AFTER_BLOCK_STAGE_FLUSH
INSTALL_AFTER_METADATA_STAGE_FLUSH
INSTALL_AFTER_BACKUPS_DURABILITY_BARRIER
INSTALL_AFTER_PREPARED_JOURNAL_DURABLE
INSTALL_BEFORE_BLOCK_REPLACE
INSTALL_AFTER_BLOCK_REPLACE
INSTALL_AFTER_BLOCK_REPLACE_DURABILITY_BARRIER
INSTALL_AFTER_BLOCK_REPLACED_JOURNAL_DURABLE
INSTALL_BEFORE_METADATA_REPLACE
INSTALL_AFTER_METADATA_REPLACE
INSTALL_AFTER_METADATA_REPLACE_DURABILITY_BARRIER
INSTALL_AFTER_PAIR_REPLACED_JOURNAL_DURABLE
INSTALL_AFTER_PAIR_VERIFY
INSTALL_AFTER_COMMITTED_JOURNAL_DURABLE
INSTALL_AFTER_TOMBSTONE_RESERVATION_DURABLE
INSTALL_AFTER_JOURNAL_RETIRE
INSTALL_RECOVERY_BLOCK_REPLACE
INSTALL_RECOVERY_METADATA_REPLACE
INSTALL_RECOVERY_AFTER_MODE_STAGE_FLUSH
INSTALL_RECOVERY_DURABILITY_BARRIER
INSTALL_RECOVERY_VERIFY
INSTALL_AFTER_LOCK_RESERVATION_DURABLE
INSTALL_AFTER_LOCK_MOVE_TO_RETIRED
INSTALL_AFTER_LOCK_DELETE_PENDING
```

Python tests inject the `durable_fs` operations/fault adapter; the two-argument
CLI always binds the real POSIX or Win32 implementation defined above.
Injection cannot replace independent JSON parsing, source/destination hashing,
phase validation, pair verification, or the no-op decision. The platform-
neutral phase matrix injects around adapter operations; the POSIX gate also
executes real file and directory barriers, while the Windows gate checks the
real binding statically and its behavior/call order with mocked Win32 results.

A byte-identical second invocation is a true pre-transaction no-op: it briefly
acquires/releases the required exclusive lock through the full identity/retry
protocol but creates no journal/stage/backup and performs no target replace.
It calls `RetireLock` for the exact path as its last filesystem action; a
retirement failure is non-success. Literal `NOOP` therefore leaves no lock/
retired artifact and preserves each target's inode/file identity where
available, bytes, mode, size, and mtime.

Expose a pure `compare_deterministic_manifests(first, second)` helper for the
real two-run gate. It independently validates both actual manifests/files,
including `0 < peakRssBytes <= 128 MiB`, requires different run IDs, directly
compares the bytes of all seven corresponding products, and requires equal
ordered hash/size tuples. Its deterministic projection normalizes only that
run-ID path segment and, after validation, replaces only
`metrics.peakRssBytes` with projection-only unsigned zero; it compares every
other DTO field and the canonical projected bytes. It never claims that the two
actual manifest byte streams are identical.

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
the fixed retired lock path
`artifacts/maps/.garner.reference-albedo.publish.lock.retired`. After the
mandatory post-acquire identity check it recovers an unfinished owned
transaction before a new build. The canonical 0600 same-directory journal is
`artifacts/maps/.garner.reference-albedo.publish.json` and its one fixed retired
path is `artifacts/maps/.garner.reference-albedo.publish.json.retired`; both
paths are resolved through the typed-reservation state table. Canonical journal
plus its exact retirement reservation is pre-move cleanup; canonical absent
plus the retired payload matching the recorded journal hash is post-move
recovery. Two journal payloads or any other combination fails closed. After
strict parse, any
`.garner.reference-albedo.publish*.retired` path not equal to the fixed lock/
journal path or an exact journal-recorded tombstone fails closed. The journal
records destination, unique temp, immutable backup, every exact typed
reservation and rollback/cleanup tombstone mapping including its fixed retired
path, prior existence/hash/mode, intended manifest hash/mode, and run ID.

This lock follows the exact handle-to-path identity/retry protocol above; a
live matching foreign owner fails closed. Under the acquired lock, startup
strictly resolves the canonical journal or its exact retired path and executes
recovery/cleanup before any input or run-directory work. Lock retirement is the
last filesystem action before release/close/return.

The journal contains one literal phase:

```text
SNAPSHOT -> PREPARED -> REPLACED -> COMMITTED
```

Every journal phase update uses `durable_fs.OpenExclusiveTemp`, stream flush
plus `FlushFile`, close, strict reparse, `ReplaceSameVolume`, and
`SyncDirectoryOrEquivalent`. The lock and every recorded path are required to
remain direct children of `options.Output`; an unexpected symlink or foreign
journal fails closed without deleting it. No publisher branch bypasses the
adapter with raw `std::filesystem::rename`, `fsync`, permissions/remove, or a
platform special case.

Write `SNAPSHOT` durably before creating a manifest temp/backup or mutating the
destination. Write JSON to a unique same-directory regular temp opened
exclusively without following a symlink; apply the intended mode/attributes to
that unpublished temp through `RestoreMode`, then flush through `FlushFile`
(`fsync` on POSIX, `FlushFileBuffers` on Windows), close, reopen, parse,
validate, mode-verify, and hash the exact temp bytes. If
the destination exists, require a non-symlink regular file, copy its exact
bytes/mode into an immutable unique same-directory backup, `FlushFile`, reopen,
verify it, and complete `SyncDirectoryOrEquivalent`. Record prior absence
without a fake backup. Only after temp/backup directory entries are durable by
the platform contract may the atomically updated journal enter `PREPARED`.

Call `ReplaceSameVolume` for the destination: POSIX atomic rename after the
same-device check, or Windows `MoveFileExW` with exactly
`MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH` after the same-volume check.
Complete `SyncDirectoryOrEquivalent` (POSIX parent fsync; the already-completed
write-through move on Windows), durably journal `REPLACED`, then reopen the top
manifest and require exact intended bytes/hash plus strict parse/validation.
Durably journal `COMMITTED` only after that readback succeeds. A successful
return requires `COMMITTED`, reports the top-manifest SHA-256 and run ID,
removes only journal-recorded owned transaction artifacts through
`RemoveOwned`, retires the canonical journal last to its exact fixed path, and
completes the platform barrier. `RetireLock` is the final filesystem action.

Any pre-commit build/gate/roundtrip/publish failure restores exact prior
bytes/mode/existence. Never consume the immutable backup directly: copy it to a
new rollback temp, apply the recorded prior mode/attributes through
`RestoreMode` while it is unpublished, then `FlushFile`, reopen, and verify the
stage. Only then `ReplaceSameVolume` the destination, complete
`SyncDirectoryOrEquivalent`, and verify prior hash/mode; for prior absence call
`RemoveOwned` with its journal-recorded tombstone, complete the same barrier,
and verify absence.
A one-shot injected failure returns `WRITE_FAILED` only after verified
rollback. Persistent rollback replace/remove/barrier/verification failure
returns `RECOVERY_REQUIRED`, retains journal and all valid recovery evidence,
and prints the exact shell-quoted full
`terrain-reference` invocation; rerunning it performs recovery before any map,
DB, alpha, catalog, or run-directory work. Startup phases `SNAPSHOT`,
`PREPARED`, and `REPLACED` restore the old state; `COMMITTED` retains and
verifies the new manifest, then finishes cleanup. Startup from only the fixed
retired journal performs the same target verification/recovery before replaying
tombstone cleanup. An unreferenced new run may remain, but no failed return
claims it through a successfully published top manifest.

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

GREEN evidence must name both distinct run IDs; direct byte equality and two
equal ordered sets of seven independently verified hashes/sizes; equal
deterministic projections after normalizing only the run-prefix and excluding
only actual `peakRssBytes`; independently valid actual RSS values in
`(0, 128 MiB]`; complete equal input-provenance hashes/mappings; the exact
published manifest path/hash; zero absent/unresolved counts; budget/geometry
metrics; the two installed runtime files; and literal `NOOP` from the second
installer invocation with unchanged file identity/bytes/mode/size/mtime. The
evidence must not claim that the two actual manifests are byte-identical.
Generated outputs and recovery artifacts stay ignored and no transaction
journal, lock, retired/tombstone, temp, or backup remains after success.

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

### Task 8: Rebuild, import, package, and attest the Garner reference terrain

Task 8 starts only after terrain Tasks 1–7 are GREEN, independently reviewed,
and committed. It consumes the one published Task 7 top manifest
`artifacts/maps/garner.reference-albedo.json`; it never discovers a run by
directory order and never trusts ignored `Content` from an earlier session.

The task has three explicit acceptance layers:

1. **Owned here:** clean-checkout terrain command/installer, deterministic
   `/Game/Maps/Garner` bootstrap, two-pass reference import, independent
   editor checker, Mac Editor/Game builds, cook/package audit, and a packaged
   NullRHI runtime smoke test.
2. **Required prerequisite for the Game/cook/runtime layer:** Movement Task 6
   has replaced `UCorsairsTerrainHeights` with the runtime
   `FCorsairsCharacterGround` and removed the Editor-only
   `CorsairsGame -> CorsairsImport` dependency. Editor-only terrain import may
   be developed before that point, but Task 8 may not claim full GREEN or a
   successful runtime package while the reverse dependency remains.
3. **Downstream, not duplicated here:** scene parity Task 8 consumes the
   atomically published terrain-base bundle and owns the instrumented original
   plus UE graphical capture at 1920x1080, 30 Hz, tick 120. A terrain-only
   marker world is not a valid visual comparison with the populated original
   city, so this task emits no fake parity screenshot and makes no visual-
   parity claim.

The implementation must preserve the exact user rule: all heavyweight
commands are blocking and sequential, run at `nice -n 10`, native/UE
parallelism is capped at two, and every owned Editor/client/process tree is
reaped immediately after its active step. No original `Game.exe` or
CrossOver/Wine process is launched by this task.

**Files:**

- Create: `CorsairsUE/Scripts/reference_terrain_rules.py`
- Create: `CorsairsUE/Scripts/build_reference_terrain_level.py`
- Create: `CorsairsUE/Scripts/import_reference_terrain.py`
- Create: `CorsairsUE/Scripts/check_reference_terrain.py`
- Create: `CorsairsUE/Scripts/tests/test_reference_terrain_rules.py`
- Create: `CorsairsUE/Scripts/tests/test_module_dependencies.py`
- Create: `CorsairsUE/Source/CorsairsImport/Private/Tests/ReferenceTerrainAssetTests.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsReferenceTerrainRuntimeTests.cpp`
- Create: `scripts/build_garner_reference_terrain.py`
- Create: `scripts/tests/test_build_garner_reference_terrain.py`
- Modify: `CorsairsUE/Scripts/setup_terrain_material.py`
- Modify: `CorsairsUE/Scripts/apply_terrain_material.py`
- Modify: `CorsairsUE/Scripts/place_terrain.py`
- Modify: `CorsairsUE/Source/CorsairsImport/CorsairsImport.Build.cs`
- Modify: `CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs`

Do not modify or stage Task 7 outputs, `Content`, `artifacts`, installed
`Data/Heights` products, `databases/game.db`, `__pycache__`, or another
agent's tracked WIP. The Task 8 commit contains only the tracked sources above.

## Exact handoff from Tasks 1–7

Before any Unreal mutation, rerun and validate the exact Task 7 chain:

```text
terrain-reference command
  -> artifacts/maps/runs/<one-run-id>/seven regular files
  -> artifacts/maps/garner.reference-albedo.json published last
  -> install_runtime_map_data.py
  -> CorsairsUE/Data/Heights/garner.block.raw
  -> CorsairsUE/Data/Heights/garner.terrain.json
```

The top manifest is schema 1 and algorithm
`legacy-fixed-pipeline-v1`. Require source map SHA-256, page `(17,21)`, source
bounds `{2176,2688,128,128}`, `pixelsPerCell=32`, dimensions `4096x4096`,
ambient `[1,1,1]`, `dwTColor=0`, required-present rect
`{2193,2756,80,47}`, a sorted unique real texture-ID set, a 16x16 section
mask rooted at `(272,336)` with exactly 256 ones, seven canonical
`runs/<one-run-id>/<expected-leaf>` paths/hashes, zero absent/unresolved
counts, and every Task 5/6 budget and geometry metric.

The seven leaves are exact and distinct:

```text
garner.height.r16
garner.block.raw
garner.region.raw
garner.terrain.json
garner.albedo_17_21.png
garner.terrain_17_21.gltf
garner.terrain_17_21.bin
```

`validate_manifest` reopens and hashes all seven files from
`manifest_path.parent`; no report-supplied hash is trusted. The installer is
run twice. The first result must make the two runtime files equal the
manifest's `block` and `terrainMetadata` hashes; the second must be a true
no-op. Installer rollback tests from Task 7 remain part of the full Python
suite.

The clean orchestrator uses the exact Task 7 production arguments before the
Editor build; it does not accept a hand-written manifest:

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
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  CorsairsUE/Scripts/install_runtime_map_data.py \
  artifacts/maps/garner.reference-albedo.json \
  CorsairsUE/Data/Heights
```

## Pure Python contracts

`reference_terrain_rules.py` imports no `unreal` module and exposes:

```python
from dataclasses import dataclass
from pathlib import Path
from typing import TypedDict


class ValidationIssue(TypedDict):
    code: str
    field: str
    detail: str


REFERENCE_GAME_MODE = "/Script/CorsairsGame.CorsairsGameMode"
REFERENCE_MAP_PACKAGE = "/Game/Maps/Garner"
REFERENCE_WORLD_OBJECT = "/Game/Maps/Garner.Garner"


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
    actor_bounds: list[tuple[str, float, float, float, float]],
) -> list[str]: ...


def assert_legacy_target_allowed(
    entry_point: str,
    asset_path: str,
) -> None: ...


def validate_level_build_report(
    data: dict,
    repo_root: Path,
) -> list[ValidationIssue]: ...
def validate_import_report(
    data: dict,
    repo_root: Path,
) -> list[ValidationIssue]: ...
def validate_idempotent_import_reports(
    first: dict,
    second: dict,
) -> list[ValidationIssue]: ...
def validate_check_report(
    data: dict,
    repo_root: Path,
) -> list[ValidationIssue]: ...
def validate_base_bundle(
    data: dict,
    bundle_path: Path,
    repo_root: Path,
) -> list[ValidationIssue]: ...
```

`asset_paths` and `reference_actor_object_path` use these exact bodies:

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

Their required outputs are:

```text
/Game/Terrain/Reference/Garner/SM_Garner_17_21
/Game/Terrain/Reference/Garner/T_Garner_17_21
/Game/Terrain/Reference/M_TerrainReference
/Game/Terrain/Reference/Garner/MI_Garner_17_21
/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21
```

The build marker path is exactly
`/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot`.
Old actor-style paths without `.Garner`, Blueprint `_C` GameModes,
`/Script/Engine.GameModeBase`, or a different map package are fatal.

## Material and texture sampling contract

The imported page is already the final legacy fixed-pipeline color. It is not
lit a second time. Require and independently read back:

```text
texture class = Texture2D
source size = 4096x4096 RGBA8
source/import metadata SHA-256 = manifest files.albedo.sha256
sRGB = true
compression = TC_DEFAULT
filter = TF_BILINEAR
address X/Y = TA_CLAMP
mip generation = TMGS_FROM_TEXTURE_GROUP
LOD group = TEXTUREGROUP_WORLD
LOD bias = 0
never_stream = false

M_TerrainReference blend = BLEND_MASKED
M_TerrainReference shading = MSM_UNLIT
TextureSample sampler type = SAMPLERTYPE_COLOR
TextureSample sampler source = SSM_FROM_TEXTURE_ASSET
TextureSample parameter name = BaseColorTexture
RGB -> MP_EMISSIVE_COLOR
A -> MP_OPACITY_MASK
material usage = MATUSAGE_STATIC_MESH + MATUSAGE_NANITE

MI parent = /Game/Terrain/Reference/M_TerrainReference
MI BaseColorTexture = T_Garner_17_21
mesh Nanite enabled = true
mesh slot 0 = MI_Garner_17_21
level component slot 0 = MI_Garner_17_21
```

The importer changes a setting only when the actual value differs. The C++
Editor test reads texture source dimensions/settings, the material expression
graph and usage flags, mesh slot, component override, Nanite state, actor
transform, and bounds from the saved/reloaded packages. Hash metadata without
property/graph readback is insufficient; property readback without manifest
file/hash validation is also insufficient. The later graphical tick-120
capture remains the final proof of sampling/display behavior.

Legacy `setup_terrain_material.py`, `apply_terrain_material.py`, and
`place_terrain.py` call `assert_legacy_target_allowed` immediately before
every asset assignment or actor mutation. Every `/Game/Terrain/Reference`
target raises `ValueError`; ordinary `/Game/Terrain/...` remains allowed.

## Module and packaged-data contract

After Movement Task 6, both forbidden dependency directions must be absent:

```text
CorsairsGame !-> CorsairsImport
CorsairsImport !-> CorsairsGame
```

`CorsairsImport` remains `Type=Editor`; move its existing `UnrealEd`
dependency from `PublicDependencyModuleNames` to
`PrivateDependencyModuleNames`. Its asset tests load the exact GameMode by
`FSoftClassPath`, include no `CorsairsGame` header, and add no reverse module
edge. This supersedes the stale canonical-plan sentence that expected
`CorsairsImport` in `CorsairsGame`: retaining that edge makes the Mac Game
target/cook invalid.

`CorsairsGame.Build.cs` stages these exact runtime inputs as UFS runtime
dependencies when present, preserving their project-relative paths:

```text
Data/character_map.json
Data/Heights/garner.block.raw
Data/Heights/garner.terrain.json
```

The clean orchestrator runs Task 7's installer before either Game build or
cook, so all three must exist for acceptance. A normal source-only Editor
build is not made permanently dependent on ignored generated files; the
packaging/runtime gate itself fails if any input is missing, has the wrong
hash, is omitted from staged output, or cannot be read through the same
`FPaths::ProjectDir()/Data/...` path used by runtime code.
`RuntimeDependencies` uses `$(ProjectDir)/<project-relative-path>` with
`StagedFileType.UFS`. The normalized stage destination is
`CorsairsUE/<project-relative-path>` and the corresponding UnrealPak member is
`../../../CorsairsUE/<project-relative-path>`; each of the three exact mappings
is asserted independently.
The staged/archive audit also requires no `CorsairsImport` runtime binary or
module descriptor; its presence is an Editor-module leak even if cook exits
zero.

## Editor report contracts

All entry points accept exactly the positional arguments from the canonical
plan, write `<report>.tmp`, flush/fsync, `os.replace`, fsync the parent, then
self-validate. The temp is created exclusively as a same-directory physical
0600 regular file and both temp/final leaves must be inside the current
transaction evidence root. They write an issue-bearing failure report and raise
`RuntimeError` on any issue. Exit code or the Unreal line `Python script
executed successfully` is never accepted without a valid report.

All report/bundle validators are strict DTO parsers, not best-effort readers. Unknown
keys, duplicate JSON keys, booleans in integer fields, non-finite numbers,
absolute or non-normalized filesystem paths, an uppercase/non-64-character SHA-256,
and an unsorted or duplicate list are fatal. `status="PASS"` requires
`issues=[]`; `status="FAIL"` requires at least one issue. An issue has exactly
this shape:

The only path-grammar exception is `ContainerMember.path`, whose literal
`../../../<normalized-mount-suffix>` form is defined below; it is never passed
to a host filesystem API or resolved as a repository path.

```text
Issue := {
  "code": nonempty string,
  "field": JSON-pointer-like absolute string,
  "detail": nonempty string
}

FileEvidence := {
  "path": normalized repository-relative POSIX path,
  "sha256": 64 lowercase hex characters,
  "sizeBytes": nonnegative integer
}

ReportEvidence := FileEvidence

ObjectChange := {
  "objectPath": absolute Unreal object path,
  "className": exact absolute native `/Script/...` class path,
  "reason": nonempty stable reason code
}
```

Every evidenced or snapshotted present file must be a physical regular file
with `nlink==1`; hard links are rejected just like symlinks and special files.
Zero bytes are allowed only for a framework-generated automation-report member;
manifests, DTO reports, receipts, build products, package primaries/sidecars,
containers, executable, and all three runtime inputs must have `sizeBytes>0`.

The five physical package families are exact. The first four have a required
`.uasset` primary; Garner has a required `.umap` primary. Each may additionally
have only same-stem `.uexp`, `.ubulk`, and `.uptnl` files. A file with any other
same-stem suffix, a symlink/special file, an omitted on-disk sidecar, or a
reported nonexistent sidecar is fatal:

```text
PackageFileEvidence := FileEvidence

PackageFamilyEvidence := {
  "family": one of "mesh", "texture", "material", "instance", "map",
  "package": exact `/Game/...` package path for that family,
  "files": nonempty list[PackageFileEvidence] sorted by path,
  "familySha256": 64 lowercase hex characters
}
```

`familySha256` is SHA-256 of UTF-8
`json.dumps(files, sort_keys=True, separators=(",", ":"),
ensure_ascii=False)` using the literal `files` array above. The list named
`finalPackageHashes` contains exactly five `PackageFamilyEvidence` records in
literal family order `mesh, texture, material, instance, map`; despite its
historical name it binds every physical file, not only five primaries. Every
producer enumerates the four allowed suffixes for each exact stem directly;
recursive discovery and parent-directory globs are forbidden.

The reusable `ReferenceState` DTO has exactly this shape and exact enum/string
values from the material contract above:

```text
ReferenceState := {
  "mesh": {
    "objectPath": "/Game/Terrain/Reference/Garner/SM_Garner_17_21",
    "sourceGltfSha256": 64 lowercase hex characters,
    "sourceBinSha256": 64 lowercase hex characters,
    "naniteEnabled": true,
    "materialSlot0":
      "/Game/Terrain/Reference/Garner/MI_Garner_17_21"
  },
  "texture": {
    "objectPath": "/Game/Terrain/Reference/Garner/T_Garner_17_21",
    "sourceWidth": 4096,
    "sourceHeight": 4096,
    "sourceFormat": "RGBA8",
    "sourceSha256": 64 lowercase hex characters,
    "srgb": true,
    "compression": "TC_DEFAULT",
    "filter": "TF_BILINEAR",
    "addressX": "TA_CLAMP",
    "addressY": "TA_CLAMP",
    "mipGenSettings": "TMGS_FROM_TEXTURE_GROUP",
    "lodGroup": "TEXTUREGROUP_WORLD",
    "lodBias": 0,
    "neverStream": false
  },
  "material": {
    "objectPath": "/Game/Terrain/Reference/M_TerrainReference",
    "blendMode": "BLEND_MASKED",
    "shadingModel": "MSM_UNLIT",
    "parameterName": "BaseColorTexture",
    "samplerType": "SAMPLERTYPE_COLOR",
    "samplerSource": "SSM_FROM_TEXTURE_ASSET",
    "rgbOutput": "MP_EMISSIVE_COLOR",
    "alphaOutput": "MP_OPACITY_MASK",
    "usageFlags": ["MATUSAGE_NANITE", "MATUSAGE_STATIC_MESH"]
  },
  "instance": {
    "objectPath":
      "/Game/Terrain/Reference/Garner/MI_Garner_17_21",
    "parent": "/Game/Terrain/Reference/M_TerrainReference",
    "baseColorTexture":
      "/Game/Terrain/Reference/Garner/T_Garner_17_21"
  },
  "actor": {
    "objectPath":
      "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21",
    "className": "/Script/Engine.StaticMeshActor",
    "label": "ReferenceTerrain_Garner_17_21",
    "tag": "CorsairsReferenceTerrain",
    "locationCm": [217600.0, -268800.0, 0.0],
    "staticMesh": "/Game/Terrain/Reference/Garner/SM_Garner_17_21",
    "boundsMinCm": two finite numbers within 1 cm of
      [217600.0, -281600.0],
    "boundsMaxCm": two finite numbers within 1 cm of
      [230400.0, -268800.0],
    "componentMaterialSlot0":
      "/Game/Terrain/Reference/Garner/MI_Garner_17_21"
  }
}
```

The level-build success report contains exactly:

```text
{
  "schemaVersion": 1,
  "reportType": "garner-reference-terrain-level-build",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "manifest": FileEvidence,
  "mapPackage": "/Game/Maps/Garner",
  "worldObject": "/Game/Maps/Garner.Garner",
  "mapPackageHash": PackageFamilyEvidence for the map family immediately
    after the builder save/reload,
  "markerObject":
    "/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot",
  "gameModeClass": "/Script/CorsairsGame.CorsairsGameMode",
  "replacedExistingMap": boolean,
  "issues": []
}
```

The import success report contains exactly:

```text
{
  "schemaVersion": 1,
  "reportType": "garner-reference-terrain-import",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "manifest": FileEvidence,
  "mapPackage": "/Game/Maps/Garner",
  "worldObject": "/Game/Maps/Garner.Garner",
  "markerObject": exact marker object path,
  "gameModeClass": "/Script/CorsairsGame.CorsairsGameMode",
  "beforeMapPackageHash": PackageFamilyEvidence observed before this pass,
  "referenceState": ReferenceState,
  "created": sorted unique list[ObjectChange],
  "updated": sorted unique list[ObjectChange],
  "deleted": sorted unique list[ObjectChange],
  "savedPackages": sorted unique list of absolute `/Game/...` package paths,
  "finalPackageHashes": five PackageFamilyEvidence records,
  "issues": []
}
```

Object-change lists sort by `(objectPath, className, reason)`. The check success
report contains exactly:

```text
{
  "schemaVersion": 1,
  "reportType": "garner-reference-terrain-check",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "manifest": FileEvidence,
  "levelBuildReport": ReportEvidence,
  "importPass1Report": ReportEvidence,
  "importPass2Report": ReportEvidence,
  "mapPackage": "/Game/Maps/Garner",
  "worldObject": "/Game/Maps/Garner.Garner",
  "markerObject": exact marker object path,
  "gameModeClass": "/Script/CorsairsGame.CorsairsGameMode",
  "referenceState": ReferenceState,
  "finalPackageHashes": five PackageFamilyEvidence records,
  "overlappingLegacyActors": sorted unique list of absolute object paths,
  "visibleLegacyOverlaps": [],
  "grassOverrides": [],
  "materialFallbackCount": 0,
  "materialUsageErrorCount": 0,
  "translucentNaniteCount": 0,
  "issues": []
}
```

Failure reports retain every exact success key and set `status="FAIL"`. A field
that could not be observed may be literal `null` only in a failure report and
only when at least one `Issue.field` equals that field's exact JSON pointer;
already observed fields keep their success type, and `issues` is nonempty. No
missing-key or truncated alternate schema is accepted. A success validator
rejects all nulls. `repo_root` must be a physical directory. Every structural
report validator reopens the manifest and report `FileEvidence` references only
below that explicit root. A producer also rehashes package-family evidence when
it writes it; a later validator treats `mapPackageHash` and
`beforeMapPackageHash` as historical observations and instead checks their
hash-chain relations. Final package evidence is independently reopened by the
checker/base validator. `bundle_path` must
resolve to `repo_root/artifacts/maps/reports/garner-terrain-base.json` or the
same-directory publication temp passed for pre-publish validation.
Every Task 8 report, inventory, build wrapper, package wrapper, journal, and
base bundle must carry the same transaction ID and source HEAD; a cross-run or
cross-commit evidence link is fatal even when bytes and an outer hash were
updated consistently.

`build_reference_terrain_level.py`:

- validates the Task 7 manifest before touching Unreal;
- accepts only `/Game/Maps/Garner`;
- deletes/replaces only the ignored generated Garner package;
- uses `LevelEditorSubsystem.new_level` and creates exactly one renamed marker
  object plus label `ReferenceTerrainBuildRoot` and tag
  `CorsairsReferenceTerrainBuildRoot`;
- loads exactly `/Script/CorsairsGame.CorsairsGameMode`, sets
  `WorldSettings.default_game_mode`, never substitutes another class;
- saves/reloads and verifies package `/Game/Maps/Garner`, world object
  `/Game/Maps/Garner.Garner`, marker path and exact GameMode;
- atomically reports manifest path/hash, map/package/world/marker/GameMode,
  the complete saved map-family file set/hash, replacement flag, and
  `issues=[]`.

The builder's marker-only world is explicitly not playable and not visual
acceptance. It contains no fabricated scene, legacy terrain, PlayerStart, or
hidden ignored prerequisite.

`import_reference_terrain.py` validates before mutation, then:

1. Loads the exact map/world/marker/GameMode and refuses any mismatch.
2. Snapshots all physical files in the five canonical package families: mesh,
   texture, base material, material instance, and Garner map. Primaries and
   every existing `.uexp`/`.ubulk`/`.uptnl` sidecar participate.
3. Imports only manifest-resolved glTF+bin+PNG whose hashes already passed.
   Any unexpected generated material/texture sidecar is deleted and reported.
   The mesh stores the manifest glTF and bin hashes as metadata; the texture
   stores the PNG hash. Idempotence compares those exact metadata values plus
   actual import/settings state before deciding whether to reimport.
4. Reconciles the exact texture/material contract above.
5. Keeps exactly one renamed `StaticMeshActor` with full object path
   `/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21`,
   label `ReferenceTerrain_Garner_17_21`, tag
   `CorsairsReferenceTerrain`, and location
   `(217600,-268800,0)` cm.
6. Requires component bounds within 1 cm of X `[217600,230400]`,
   Y `[-281600,-268800]` cm.
7. Uses actual `get_actor_bounds(False)` and strict four-edge overlap. It
   persistently hides/tags only overlapping legacy `Terrain_*` actors and
   restores any tagged actor that no longer overlaps.
8. Saves only dirty packages and computes hashes after successful saves.

Each import report uses the literal schema above. Pass 2 must have all four
mutation/save lists empty and byte-identical `ReferenceState`, physical file
sets, sizes, per-file hashes, and five family digests from pass 1. Adding,
removing, or changing only a sidecar is therefore a mutation and is fatal.
Pass 1 `beforeMapPackageHash` must equal the level-build `mapPackageHash`; pass
2 `beforeMapPackageHash` must equal pass 1's final map-family record.

`check_reference_terrain.py` is read-only. It validates and hashes the
manifest, level-build report, both import reports and idempotence relation;
loads the exact world; independently inspects the actual marker, GameMode,
assets, texture sampling, material graph/usages, mesh/Nanite/material slots,
one actor/transform/bounds, all legacy overlaps/tags/visibility, and every
physical file in all five package families. It never creates, mutates, dirties,
deletes, or saves a package. Success requires actual family file lists, sizes,
hashes, and family digests exactly equal pass 2 and
zero issues/visible overlaps/grass overrides. Its observations also require
literal counts `materialFallbackCount=0`, `materialUsageErrorCount=0`, and
`translucentNaniteCount=0`; a WorldGrid/default/white fallback or a material
that merely compiles without both required usages is fatal.

## Durable rollback and base-bundle publication

`scripts/build_garner_reference_terrain.py` is the sole clean-checkout Task 8
orchestrator. Pure tests inject the command runner, file operations and
failpoints. Its transaction paths are literal and all remain ignored:

```text
artifacts/maps/reports/.garner-terrain-task8.lock
artifacts/maps/reports/.garner-terrain-task8.transaction.json
artifacts/maps/reports/.garner-terrain-task8.recovery/<transaction-id>/
artifacts/maps/reports/runs/<transaction-id>/
artifacts/maps/package-run/<transaction-id>/
```

`transaction-id` is exactly 32 lowercase hexadecimal characters generated from
16 random bytes; all transaction recovery/evidence/package directories are
created with exclusive, no-follow semantics, mode 0700, and a collision is
retried. The output root and every recovery/run
parent are physical directories. The lock is a persistent 0600 physical
regular file containing exactly
`{"schemaVersion":1,"lockName":"garner-terrain-task8"}\n`. The orchestrator
opens it with no-follow/create semantics, verifies descriptor/path identity and
`nlink==1`, takes nonblocking exclusive `flock`, and keeps that descriptor until
its final filesystem action. While holding the first lock, a newly created or
incompletely written marker (possible only before any durable journal) is
truncated and rewritten through the locked descriptor, flushed, fsynced,
reopened/read back through the path, and its parent is fsynced. A mismatched
marker with an existing journal/recovery transaction is fail-closed. The lock path is never
unlinked, so there is no old-inode/new-path race. A busy lock means another
Task 8 transaction and is a non-mutating failure; stale text is not treated as
ownership evidence.

After taking the lock, every normal invocation performs startup recovery before
process/thermal checks or any new transaction. `--recover-only` performs only
that recovery and exits. The exact manual command printed with
`RECOVERY_REQUIRED` is:

```bash
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  scripts/build_garner_reference_terrain.py \
  --repo-root "$PWD" \
  --manifest artifacts/maps/garner.reference-albedo.json \
  --map /Game/Maps/Garner \
  --output artifacts/maps/reports \
  --recover-only
```

The journal is strict JSON with no unknown/duplicate keys and exactly this
shape:

```text
Snapshot := {
  "path": normalized repository-relative POSIX path,
  "priorType": one of "absent", "regular",
  "priorMode": integer in [0, 4095] or 0 when absent,
  "priorSha256": 64 lowercase hex or "" when absent,
  "priorSizeBytes": nonnegative integer or exactly 0 when absent,
  "backupPath": normalized path below this transaction recovery root or ""
}

ActiveProcess := null or {
  "step": one exact `completedStep` name below or the recovery-only
    "nested-publisher-recovery" or "nested-installer-recovery",
  "pid": positive integer,
  "pgid": positive integer,
  "startToken": nonempty OS process-start identity,
  "executable": normalized absolute physical path,
  "argvSha256": SHA-256 of NUL-delimited exact argv bytes
}

PhysicalDirIdentity := [device, inode, owner UID, permission mode], all exact
  nonnegative integers with positive inode

SandboxScratch := null or {
  "state": one of "PLANNED", "RESERVED", "PREPARED", "COPIED",
    "CLEANING", "CLEANED",
  "bundleIdentifier": the exact `CFBundleIdentifier` derived from the archived
    signed app and accepted by the rules below,
  "containerRoot": normalized absolute physical path to the matching
    current-user application container,
  "containerRootIdentity": PhysicalDirIdentity,
  "containerDataRoot": normalized absolute physical path to the matching
    current-user application-container `Data` directory,
  "containerDataIdentity": PhysicalDirIdentity,
  "scratchParent": the exact physical automation reports root returned by the
    signed packaged probe,
  "scratchParentIdentity": PhysicalDirIdentity,
  "reservationPath": exact
    `<scratchParent>/.CorsairsTerrainTask8-<same-transaction-id>.reservation`,
  "reservationIdentity": Physical regular-file identity
    `[device,inode,owner UID,permission mode,size]` in `RESERVED` and later, or
    [] in `PLANNED`,
  "reservationSha256": expected 64-lowercase-hex typed-reservation hash,
  "transactionRoot": exact
    `<scratchParent>/CorsairsTerrainTask8-<same-transaction-id>`,
  "reportRoot": exact `<transactionRoot>/reference-terrain-runtime`,
  "transactionIdentity": PhysicalDirIdentity after its creation, or [] before,
  "reportIdentity": PhysicalDirIdentity in `PREPARED` and later, or [] before,
  "markerSha256": expected 64-lowercase-hex owner-marker hash in every state
}

{
  "schemaVersion": 1,
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex equal to the clean checkout HEAD,
  "phase": one of "SNAPSHOT", "TASK7_PUBLISHED", "RUNTIME_INSTALLED",
    "UNREAL_RUNNING", "EDITOR_VERIFIED", "PACKAGE_VERIFIED",
    "BUNDLE_PREPARED", "BUNDLE_REPLACED", "COMMITTED", "ROLLED_BACK",
  "completedStep": one of "snapshot", "terrain-reference", "installer-1",
    "installer-2", "editor-build", "level-build", "import-1", "import-2",
    "checker", "editor-automation", "game-build", "cook-package",
    "runtime-report-probe", "runtime-smoke", "base-validated", "base-published",
  "evidenceRoot":
    "artifacts/maps/reports/runs/<same-transaction-id>",
  "packageRunRoot":
    "artifacts/maps/package-run/<same-transaction-id>",
  "recoveryRoot":
    "artifacts/maps/reports/.garner-terrain-task8.recovery/<same-id>",
  "snapshots": sorted unique list[Snapshot],
  "managedFamilyListings": {
    "mesh": sorted exact pre-run suffix paths,
    "texture": sorted exact pre-run suffix paths,
    "material": sorted exact pre-run suffix paths,
    "instance": sorted exact pre-run suffix paths,
    "map": sorted exact pre-run suffix paths
  },
  "activeProcess": ActiveProcess,
  "sandboxScratch": SandboxScratch,
  "intendedBundleSha256": 64 lowercase hex or "",
  "issues": []
}
```

Before creating a new transaction, the orchestrator resolves
`git rev-parse --show-toplevel` to the same physical `repo_root`, requires a
40-lowercase-hex `git rev-parse HEAD`, and requires empty output from
`git status --porcelain=v1 --untracked-files=all`; ignored generated paths stay
excluded by Git and are handled only by this transaction. The resulting
`sourceHead` is written in the first journal and every top-level Task 8 report,
inventory, evidence wrapper, package wrapper, and base bundle. Startup
recovery performs the same read-only identity check and requires the current
HEAD to equal the journal `sourceHead`; a dirty/different checkout is
`RECOVERY_REQUIRED` before target mutation and reports the required commit.

The journal is rewritten through a unique 0600 same-directory physical temp,
file flush+fsync, strict readback, `os.replace`, and parent fsync. Snapshot
backups are 0600 physical regular files on the same volume; each is flushed,
fsynced, reopened, rehashed, and its recovery directory is fsynced before the
first `SNAPSHOT` journal becomes durable. Snapshot paths include the exact
previous bytes/existence/mode/hash of the current top terrain manifest, the two
installed runtime files, the prior terrain-base bundle, and all allowed files
in the five managed package families. Pre-run exact family listings make every
new sidecar removable without using a parent directory as a destructive target.
For an absent snapshot, mode/size are zero and hash/backup are empty; for a
regular snapshot, hash/backup are nonempty and backup size/hash equal the prior
record exactly.

Rollback never consumes a backup. For each prior regular file it creates an
exclusive physical restore temp in the target's own directory, copies the
backup, applies the recorded mode to the temp, flushes+fsyncs+reopens+rehashes
it, `os.replace`s the exact target, then fsyncs the target parent. For each
prior-absent file or newly introduced allowed sidecar it unlinks only a verified
physical regular leaf and fsyncs that parent; already absent is idempotent.
Any directory, symlink, special file, hard link, escape, unexpected same-stem
suffix, or failed parent fsync stops recovery with the journal/backups intact.
After the ordered restore, it re-enumerates every family and rechecks all prior
bytes, hashes, sizes, modes, and absences before writing `ROLLED_BACK`.

All other mutable evidence is transaction-private rather than overwritten.
Level/import/check reports, both automation-report trees, synthesized
cook/package/runtime reports, copied Editor/Game target receipts, and copies of
every project-owned regular `BuildProduct` named by those receipts live only
under `reports/runs/<transaction-id>`. Cook/stage/archive output lives only
under `package-run/<transaction-id>`. The bundle never names mutable receipt,
binary, or fixed report paths under `CorsairsUE/Binaries` or the shared reports
root; it names the verified run-private copies. Consequently restoring a prior
bundle cannot leave it pointing at reports, receipts, or binaries overwritten
by a failed later run. Failed run-private evidence and a newly created Task 7
run may remain unreferenced, but no prior bundle input changes.

Task 8 is an outer transaction around two independently journaled Task 7
owners. It never restores the top manifest or runtime pair across an unresolved
inner publisher/installer transaction. The exact inner control paths are:

```text
artifacts/maps/.garner.reference-albedo.publish.lock
artifacts/maps/.garner.reference-albedo.publish.lock.retired
artifacts/maps/.garner.reference-albedo.publish.json
artifacts/maps/.garner.reference-albedo.publish.json.retired
CorsairsUE/Data/Heights/.garner-runtime-install.lock
CorsairsUE/Data/Heights/.garner-runtime-install.lock.retired
CorsairsUE/Data/Heights/.garner-runtime-install.transaction.json
CorsairsUE/Data/Heights/.garner-runtime-install.transaction.json.retired
```

After the outer snapshot is durable, every Task 7 publisher/installer child is
started through the recorded process handshake below. On ordinary failure or
startup recovery, the outer orchestrator first proves the recorded child gone,
then resolves inner owners in publisher-before-installer order. Absence of all
four exact control paths for an owner means that owner has no recoverable
transaction. If any control path is present, Task 8 invokes that owner's exact
original command as `nested-publisher-recovery` or
`nested-installer-recovery`; it never parses, deletes, or edits an inner
journal itself. The owner must acquire its own lock, run its own startup
recovery, return success, leave all four control paths absent, and expose a
strictly valid current top manifest or installed pair. Because the Task 7
publisher has no recovery-only CLI, its documented full retry may publish an
unreferenced fresh run; that is recovery work, and the outer snapshot is still
restored afterward. Installer recovery then uses the current valid top
manifest before the outer pair is restored.

An inner retry uses the same durable `ActiveProcess` handshake and process
identity rules as production. Since the publisher retry may be heavyweight,
even `--recover-only` refuses to launch it unless the exact pre-existing-
process and healthy three-line `pmset` gates pass; failure leaves every outer
snapshot and journal intact as `RECOVERY_REQUIRED`. A busy/malformed/foreign
inner lock or journal, nonzero retry, remaining exact control path, or invalid
post-retry target likewise stops before any outer restore. After both owners
are proven clean, Task 8 may restore its own snapshot. It does not start a new
Task 8 transaction in `--recover-only` mode and never treats an inner retry's
fresh manifest/runtime result as the outer committed result.

Before the first mutation the orchestrator also refuses any pre-existing
`UnrealEditor`, `UnrealEditor-Cmd`, packaged client, `Game.exe`, CrossOver/Wine,
`Build.sh`, `RunUAT`, or active CMake/UBT build; it never kills such a process.
It then runs and validates Task 7 and every later subprocess in a dedicated
process group. A tiny launcher routine inside this same tracked orchestrator
forks, creates the child session, and blocks on an inherited pipe before
`execve`; no unlisted helper file or generated script participates. The parent
records its PID/PGID/start token and intended exact executable plus
NUL-delimited argv hash as `ActiveProcess`,
durably writes the journal, then
sends the one-byte exec release. EOF before release makes the launcher exit, and
successful `execve` preserves PID/PGID/start token. The parent waits in the
foreground, clears `ActiveProcess` durably after wait, and on
timeout/failure/interruption terminates and reaps only that owned group before
rollback.

The five package families are resolved narrowly from these stems; no parent
directory is ever used as a destructive target:

```text
CorsairsUE/Content/Terrain/Reference/Garner/SM_Garner_17_21
CorsairsUE/Content/Terrain/Reference/Garner/T_Garner_17_21
CorsairsUE/Content/Terrain/Reference/M_TerrainReference
CorsairsUE/Content/Terrain/Reference/Garner/MI_Garner_17_21
CorsairsUE/Content/Maps/Garner
```

The ordered mutation path is:

```text
Task 7 manifest+installer -> Editor build
       -> builder -> import pass 1 -> import pass 2 -> read-only checker
       -> Editor automation -> Game build
       -> clean cook/package (reuse the just-built receipt)
       -> packaged sandbox-path probe -> packaged NullRHI runtime smoke
```

If any command, report, hash, save, automation, cook, or runtime gate fails,
the orchestrator first reaps its owned process tree, resolves and verifies both
inner Task 7 owners by the protocol above, then restores all old
top-manifest/runtime-data/base-bundle/package-family bytes and modes or removes
files that were previously absent, removes every newly introduced managed-root
sidecar, verifies exact hashes/absence, and leaves the journal/recovery
material in place if verification fails. Success deletes recovery material
only after all reports and final files hash correctly.

Phase transitions are durable and monotonic. `SNAPSHOT` precedes the Task 7
command; `TASK7_PUBLISHED` follows independent top-manifest rehash;
`RUNTIME_INSTALLED` follows installer 1 result `OK` or `NOOP`, mandatory
installer 2 result `NOOP`, and runtime-pair
rehash; `UNREAL_RUNNING` covers each Editor/build/cook/runtime child with the
exact completed step; `EDITOR_VERIFIED` follows checker+Editor automation;
`PACKAGE_VERIFIED` follows Game build, package audit, packaged sandbox probe,
durable report copy/cleanup, and packaged runtime smoke; `BUNDLE_PREPARED`
records the validated temp hash before replace;
`BUNDLE_REPLACED` follows bundle replace plus reports-directory fsync; and
`COMMITTED` is written only after reopening and validating the published bundle
and every transitive input and after repeating the exact clean-checkout HEAD/
status gate. `ROLLED_BACK` is recovery-only and is written only
after the complete prior snapshot and family listings have been restored and
revalidated; production never transitions to it.

### Sandboxed packaged-runtime report handoff

The archived Mac application remains signed with the sandbox enabled; Task 8
must not re-sign it, remove or weaken an entitlement, or launch a different
binary. Before runtime launch, the orchestrator maps the receipt-selected
archive executable to its one enclosing `.app`, verifies that app with
`/usr/bin/codesign --verify --deep --strict`, strictly parses exactly one plist
from `/usr/bin/codesign -d --entitlements :-`, and requires the boolean
`com.apple.security.app-sandbox=true`, and obtains exactly one signing
`Identifier` from `/usr/bin/codesign -d --verbose=4`. It strictly parses that same app's
physical `Contents/Info.plist` and obtains one nonempty `CFBundleIdentifier`;
the identifier may contain only ASCII letters, digits, `.`, and `-`, may not
begin or end with `.`, and has no empty component. A hard-coded bundle
identifier, an environment-provided container path, an unsigned archive app,
or a second app candidate is failure.

The container path is derived rather than guessed. Before creating any scratch,
the same inventory-bound executable is run once as the owned
`runtime-report-probe` step with `-CorsairsTerrainSandboxProbe`, the same
transaction/source arguments, and no `-ReportExportPath`. The existing
`Corsairs.Terrain.ReferenceRuntime` test takes that mode before loading gameplay
assets and emits exactly one canonical
`CORSAIRS_TERRAIN_SANDBOX_JSON=` event. Its strict DTO contains only schema
version 1, report type `garner-terrain-packaged-sandbox`, status `PASS`, the
same transaction/source identities, the nonempty bundle ID from
`FPlatformProcess::GetGameBundleId()`, the normalized absolute no-trailing-slash
container `Data` root from `FPlatformProcess::UserHomeDir()`, the normalized
absolute no-trailing-slash reports root from `FPaths::AutomationReportsDir()`,
and empty `issues`. Zero, duplicate, noncanonical, or mismatched events fail.

Before emitting that event, the signed sandboxed probe requires the reported
container `Data` root to exist and bootstraps only the exact nonempty relative
suffix leading to `FPaths::AutomationReportsDir()`. On Mac it opens `Data`
without following a symlink in any absolute-path component and walks each
suffix component with no-follow descriptor-relative operations,
creates only a missing component with mode `0700`, requires every existing or
new component to belong to the effective UID and not be group/world writable,
records all permission bits so the reopen detects any special-bit change,
flushes every opened child and its parent including a component retained from
an interrupted prior bootstrap, then reopens and verifies the same full
identity chain before `PASS`. A non-Mac probe uses the physical platform file
for the same suffix-only operation.
Missing `Data`, a symlink or non-directory collision, unsafe ownership/mode, a
non-`ENOENT` error, or any durability or reopen mismatch emits no sandbox event.
The created automation suffix is
persistent probe-owned application state: the host never creates it, and
`SandboxScratch` cleanup never owns or removes it or any partial suffix left by
a failed probe; a later probe must revalidate that state.

The host requires the probed bundle ID to equal both the archived
`CFBundleIdentifier` and the signing `Identifier`, requires the reported
container root to be a physical directory whose leaf is exactly `Data`, and
requires the reported automation root to be a strict descendant of that root.
It walks only those exact
named components and requires each to be a no-follow physical directory owned
by the effective UID and not group/world writable. It also strictly parses the
physical non-hard-linked regular
`<container-parent>/.com.apple.containermanagerd.metadata.plist`; both
`MCMMetadataIdentifier` and `MCMMetadataCreator` must equal the same signed
bundle ID. These OS-reported roots and the archived metadata/signature are the
sole derivation inputs: Task 8 does not interpolate `$HOME`, hard-code a user or
bundle ID, or enumerate sibling containers. The host does not create a missing
application container or reported automation root, follow a symlink, or fall
back to the repository path.
The exact container, `Data`, automation-root device/inode/owner/mode identities
are reopened and compared before every scratch mutation.

`SandboxProbeEvent` is a strict in-memory control DTO, not a base-bundle input.
Its absolute values may persist only in the existing 0600 transaction-private
`commands/runtime-report-probe.{stdout,stderr}.log`,
`commands/runtime-smoke.{stdout,stderr}.log`, and the active outer journal
needed for exact recovery; those ignored local artifacts are never copied into a
report DTO, package wrapper, attestation, or base bundle. Every durable linked
report uses only the sanitized relative/leaf fields below. Validators and
mutation tests treat the raw probe parser separately from nested bundle
validation and reject any linked `SandboxProbeEvent` or absolute container/home
path.

A canonical run-private `package/app-sandbox.json` records the signed app path,
bundle identifier, `Info.plist` `FileEvidence`, the canonical strict entitlement
plist `FileEvidence`, `codesignVerified=true`, `appSandbox=true`, and the container metadata
identifier and creator observed before launch. It never copies or embeds the
private container-manager metadata plist or any unfiltered user-container
bytes. The cook report
and package evidence contain the same `FileEvidence` for this strict
attestation, and the base bundle reaches it through both nested validators. The
attestation never publishes the absolute user-container path or user home.

While the Task 8 lock is held, the orchestrator first durably journals a
`PLANNED` `SandboxScratch` with all three trusted parent identities, empty child
identities, and the deterministically expected typed-reservation/owner-marker
hashes. It then exclusively creates its exact 0600 sibling reservation file,
writes canonical JSON plus newline with exactly `schemaVersion=1`,
`owner="garner-terrain-task8-reservation"`, the same transaction/source/bundle
identities, and the exact transaction/report paths, flushes,
fsyncs, reopens/re-hashes it, synchronizes the automation root, and durably
journals `RESERVED` with its regular-file identity. Only then may it exclusively
create the exact `CorsairsTerrainTask8-<transaction-id>` root below the probed
automation reports root as a 0700 current-UID physical directory on the
parent's device; it rewrites `RESERVED` durably with that directory identity
before creating any child. Any pre-existing leaf
of any type, symlink, foreign owner, non-0700 child, identity change,
cross-device child, or nonempty report child fails closed and is never removed.
The transaction root contains one 0600 physical non-hard-linked `owner.json`.
Its bytes are the canonical JSON serialization plus newline of exactly
`schemaVersion=1`, `owner="garner-terrain-task8"`, the same transaction ID,
and the signed app's same bundle identifier. It is written exclusively,
flushed, fsynced, and reopened/rehashed. The exact
`reference-terrain-runtime` child is then exclusively created as a 0700
current-UID physical directory, and the transaction root is fsynced before the
journal may enter `PREPARED` with its exact identity. The packaged runtime
receives only the resulting absolute `reportRoot` as `-ReportExportPath`.

After the owned runtime process exits successfully and the one canonical
runtime JSON event validates, the host requires a nonempty physical
`reportRoot/index.json`, performs a strict no-follow full-tree scan with hard
links, unreadable entries, sockets/devices, escapes, duplicate normalized
paths, owner changes, and directory/file mutation rejected, and hashes every
file. It then exclusively creates the canonical run-private
`reports/runs/<transaction-id>/reference-terrain-runtime` as 0700 and recreates
each source directory/file without following links. Each destination file is
exclusive, receives the exact source permission bits, is flushed/fsynced and
reopened; destination directories are fsynced bottom-up. A second source scan
and a complete destination scan must have identical sorted relative paths,
bytes, sizes, modes, and SHA-256 values. Only then may the durable journal enter
`COPIED`, and only this verified canonical copy becomes the `EvidenceSet` and
base-bundle input.

Scratch cleanup is identity-bound and exact. In `PLANNED`, an absent root is a
no-op and the only tolerated crash residue is the absent or exact expected
typed reservation; no transaction root is accepted before that reservation
verifies. In `RESERVED`, the exact durable reservation must match, and a root
whose identity was not yet persisted may be absent or may be the exact empty
0700 current-UID directory on the recorded parent device. A root with a
persisted identity must exist with that identity. The recorded root may contain
only the absent or exact durable marker and the absent or exact empty 0700
report child; recovery removes only those deterministic partial states. In
`PREPARED` or `COPIED`, recovery requires all exact trusted/root/report identities
and the exact durable owner marker, scans every descendant no-follow, and
removes only that one journaled transaction tree bottom-up. Before the first
remove it durably enters `CLEANING`. It then synchronizes the probed automation
reports root, removes the verified reservation last, synchronizes the parent
again, verifies both exact paths absent, and durably enters
`CLEANED`; `CLEANING` accepts either the exact remaining owned suffix or an
already absent root, and `CLEANED` accepts only absence. Finally it durably
resets `sandboxScratch` to null. Thus crashes after reservation/root/marker
creation, after report
creation, during removal, after root removal, or after parent sync all replay
without guessing. A marker mismatch, unexpected descendant type, identity
mismatch, missing root outside `PLANNED`, pre-root `RESERVED`, `CLEANING`, or
`CLEANED`, or inability to synchronize
cleanup is `RECOVERY_REQUIRED` and preserves outer journal/repository evidence.
Startup recovery performs this cleanup after any recorded child is proven gone
and before inner-owner or snapshot rollback; ordinary failure does the same.
Neither rollback nor success completes while a non-null scratch record remains.

Startup recovery under the lock is deterministic:

- no journal means no active transaction. If a current terrain-base bundle
  exists, its strict syntax plus exact transaction ID/run-evidence/package-run
  roots and their immutable hashes are validated for ownership classification;
  those exact roots are not orphans even if a later external Task 7 run made a
  fixed top/package-family input stale. This classification is not current
  bundle acceptance and never authorizes deletion. Every other unreferenced
  recovery/evidence/package path is retained and reported, never guessed or
  recursively deleted;
- a malformed journal, foreign path, missing/mismatched backup, unproven live
  process identity, symlink/special file, or hash/mode mismatch returns
  `RECOVERY_REQUIRED` without mutation and prints the literal command above;
- an exactly recorded live group is allowed to finish only while its original
  orchestrator still owns the busy lock. After a stale-lock acquisition, a
  still-live exact PID/PGID/start-token/executable/argv is terminated and verified
  gone as recorded Task 8 ownership. An absent PID, or the same PID with a
  different start token, proves the recorded child is gone and recovery
  proceeds without signaling the absent/reused process; an unreadable or only
  partially matching identity is never killed and is `RECOVERY_REQUIRED`;
- after the recorded child is gone, any nonterminal outer phase resolves the
  exact sandbox scratch state by the protocol above, then resolves the exact
  Task 7 publisher and installer control paths in that order before
  restoring a top manifest or runtime target. Inner recovery is never skipped
  merely because the outer phase has advanced past `TASK7_PUBLISHED` or
  `RUNTIME_INSTALLED`; a failed inner recovery retains the outer journal and
  backups and returns `RECOVERY_REQUIRED`;
- every nonterminal production phase before `COMMITTED`, including
  `BUNDLE_REPLACED`, restores the
  snapshot, exact modes and family listings, verifies them, then durably writes
  `ROLLED_BACK`. `ROLLED_BACK` cleanup is idempotent and no longer needs backup
  bytes: it removes recovery material, verifies absence, then unlinks the
  journal and fsyncs the reports parent;
- `COMMITTED` revalidates the intended published bundle and transitive evidence,
  idempotently removes recovery material, verifies absence, then unlinks the
  journal and fsyncs the reports parent. A mismatch is `RECOVERY_REQUIRED`; it
  never silently rolls back an already committed run. A crash during cleanup
  leaves either `ROLLED_BACK` or `COMMITTED`, so the next invocation can resume
  without requiring an already deleted backup.

The orchestrator exposes both ordinary failure and simulated-crash actions at
each of these exact seams: during lock-marker truncate/write/fsync and after
lock-marker readback; after every snapshot file
fsync; after recovery-directory fsync; after `SNAPSHOT`; after Task 7; after
installer 1 and installer 2; before and after each nested-owner control-path
scan, preflight, retry launch, retry wait, control-path absence check, and
post-retry target validation; after `runtime-report-probe`, sandbox-plan
durability, reservation create/fsync/readback/parent-sync, first `RESERVED`,
transaction-root creation, identity-bearing `RESERVED`, owner-marker
fsync/readback, report-root creation, and `PREPARED`
durability; after each runtime-report destination file fsync, after all copy
directory barriers and source/destination revalidation, and after `COPIED`;
before and after `CLEANING`, each scratch descendant removal, transaction-root
removal, reservation removal, each automation-root synchronization, `CLEANED`, and the null scratch
record becoming durable; after each Editor/build/automation/cook/runtime step; after each
package-family save/listing; after bundle-temp fsync/readback;
immediately before and after bundle `os.replace`; after reports-parent fsync;
after `BUNDLE_REPLACED`; after published-bundle validation; after `COMMITTED`;
after `ROLLED_BACK`; and during each restore or cleanup replace/remove/fsync. A crash action performs no
in-process cleanup; the next fresh invocation must exercise startup recovery.
The crash matrix kills the child at every Task 7 publisher phase and every
installer phase reachable through the injected runners, then proves that a
fresh outer process either completes owner recovery before byte-exact outer
rollback or returns `RECOVERY_REQUIRED` without touching an owner-controlled
target. It also covers a crash after an inner retry succeeds but before
`ActiveProcess` is cleared; the next run observes the child gone and the four
control paths absent, then completes the same outer rollback.
The crash matrix also covers absent, `PLANNED`, pre/post-marker `RESERVED`,
`PREPARED`, `COPIED`, `CLEANING`, post-remove/pre-parent-sync, and
`CLEANED`/pre-null scratch states. A
fresh invocation either removes only the exact owned scratch and completes the
same outer rollback or returns `RECOVERY_REQUIRED`; foreign or pre-existing
container paths and marker/identity changes remain byte-for-byte untouched.

Only after the complete owned chain passes does it atomically publish
`artifacts/maps/reports/garner-terrain-base.json`. That bundle contains
normalized paths and SHA-256 for:

```text
Task 7 top manifest and its one run ID
the seven Task 7 run files
the two installed runtime files
level-build report
import pass 1 report
import pass 2 report
terrain checker report
Editor automation report
all files and five digests of the five final package families
run-private Mac Editor and Game receipts and project build products
cook/package report and packaged executable
signed-app sandbox attestation
packaged runtime automation and strict observation report
```

Publication uses a unique regular temp file in the same directory, flush,
file fsync, strict readback, `os.replace`, and parent-directory fsync. The prior
bundle is already in the durable snapshot. Any failure before `COMMITTED`
restores and revalidates its exact bytes; no post-replace fsync failure can
masquerade as a preserved old bundle.

The remaining literal bundle DTOs are:

```text
EvidenceSet := {
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "root": normalized repository-relative physical directory,
  "files": sorted nonempty list[FileEvidence]
}

BuildEvidence := {
  "transactionId": 32 lowercase hex,
  "target": one of "CorsairsUEEditor", "CorsairsUE",
  "platform": "Mac",
  "configuration": "Development",
  "sourceHead": 40 lowercase hex characters,
  "receipt": run-private FileEvidence,
  "products": sorted nonempty list of run-private FileEvidence
}

PackagedRuntimeFile := {
  "projectRelativePath": one of "Data/character_map.json",
    "Data/Heights/garner.block.raw", "Data/Heights/garner.terrain.json",
  "containerPath": normalized path of the listed `.pak` below this
    transaction stage/archive root,
  "containerMemberPath": exact normalized UFS member path,
  "extractedEvidence": run-private FileEvidence,
  "sourceSha256": 64 lowercase hex characters,
  "sourceSizeBytes": positive integer,
  "runtimeReportedSha256": the same 64 lowercase hex,
  "runtimeReportedSizeBytes": the same positive integer
}

SandboxProbeEvent := {
  "schemaVersion": 1,
  "reportType": "garner-terrain-packaged-sandbox",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "bundleIdentifier": nonempty validated signed bundle identifier,
  "containerDataRoot": normalized absolute no-trailing-slash physical path,
  "automationReportsRoot": normalized absolute no-trailing-slash physical path
    strictly below `containerDataRoot`,
  "issues": []
}

SandboxAttestation := {
  "schemaVersion": 1,
  "reportType": "garner-terrain-app-sandbox",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "appBundlePath": normalized repository-relative physical `.app` directory
    enclosing the receipt-selected packaged executable,
  "bundleIdentifier": the exact validated `CFBundleIdentifier`,
  "signingIdentifier": the same bundle identifier,
  "infoPlist": FileEvidence for that app's `Contents/Info.plist`,
  "entitlementsPlist": run-private FileEvidence for the canonical strict plist
    parsed from the archived app's signing information,
  "codesignVerified": true,
  "appSandbox": true,
  "containerDataLeaf": "Data",
  "automationReportsRelativePath": nonempty normalized relative path from the
    probed container `Data` root,
  "containerMetadataIdentifier": the same bundle identifier,
  "containerMetadataCreator": the same bundle identifier,
  "issues": []
}

PackageEvidence := {
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "targetReceipt": run-private FileEvidence,
  "stageManifest": run-private FileEvidence for the canonical synthesized
    stage/container inventory,
  "archiveManifest": run-private FileEvidence for the canonical synthesized
    archive inventory,
  "containers": sorted nonempty list[FileEvidence],
  "containerLists": sorted nonempty list[FileEvidence],
  "packagedExecutable": FileEvidence below this transaction archive root,
  "sandboxAttestation": run-private FileEvidence for SandboxAttestation,
  "runtimeFiles": exactly three PackagedRuntimeFile records in the literal
    project-relative order above
}

InventoryReport := {
  "schemaVersion": 1,
  "reportType": one of "garner-terrain-stage-inventory",
    "garner-terrain-archive-inventory",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "root": normalized repository-relative physical directory below this
    transaction package-run root,
  "files": sorted nonempty list[FileEvidence] for every physical regular file
    below that exact root, sorted by path,
  "issues": []
}

ContainerMember := {
  "path": UnrealPak member beginning exactly `../../../`, followed by a
    nonempty normalized mount-relative suffix with no further `.` or `..`,
  "sizeBytes": nonnegative integer
}

ContainerListReport := {
  "schemaVersion": 1,
  "reportType": "garner-terrain-container-list",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "container": FileEvidence,
  "members": sorted nonempty list[ContainerMember] by path,
  "issues": []
}

CookRuntimeFile := {
  "projectRelativePath": one of the three literal UFS inputs,
  "containerPath": exact `FileEvidence.path` of one listed container,
  "containerMemberPath": exact mapped UFS member path,
  "extractedEvidence": run-private FileEvidence,
  "sourceSha256": 64 lowercase hex,
  "sourceSizeBytes": positive integer
}

CookPackageReport := {
  "schemaVersion": 1,
  "reportType": "garner-terrain-cook-package",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "targetReceipt": run-private FileEvidence,
  "stageManifest": run-private FileEvidence for an InventoryReport,
  "archiveManifest": run-private FileEvidence for an InventoryReport,
  "containers": sorted nonempty list[FileEvidence],
  "containerLists": sorted nonempty list[FileEvidence], each naming a
    ContainerListReport,
  "packagedExecutable": FileEvidence below this transaction archive root,
  "sandboxAttestation": the same run-private FileEvidence as PackageEvidence,
  "runtimeFiles": exactly three CookRuntimeFile records in literal UFS order,
  "corsairsImportLeaks": [],
  "issues": []
}

RuntimeInputObservation := {
  "projectRelativePath": one of the three literal UFS inputs,
  "sha256": 64 lowercase hex,
  "sizeBytes": positive integer
}

RuntimeObservation := {
  "schemaVersion": 1,
  "reportType": "garner-terrain-packaged-runtime",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "mapPackage": "/Game/Maps/Garner",
  "worldObject": "/Game/Maps/Garner.Garner",
  "gameModeClass": "/Script/CorsairsGame.CorsairsGameMode",
  "actorObject": exact reference actor object path,
  "runtimeInputs": exactly three RuntimeInputObservation records in literal
    UFS order,
  "issues": []
}

{
  "schemaVersion": 1,
  "reportType": "garner-terrain-base",
  "status": "PASS",
  "transactionId": 32 lowercase hex,
  "sourceHead": 40 lowercase hex,
  "terrainManifest": {
    "top": FileEvidence for artifacts/maps/garner.reference-albedo.json,
    "runId": exact one-run ID from the top manifest,
    "runFiles": seven FileEvidence records in canonical leaf order
  },
  "runtimeInputs": three FileEvidence records in order character-map, block,
    terrain-metadata,
  "reports": {
    "levelBuild": run-private ReportEvidence,
    "importPass1": run-private ReportEvidence,
    "importPass2": run-private ReportEvidence,
    "terrainCheck": run-private ReportEvidence,
    "editorAutomation": EvidenceSet,
    "cookPackage": run-private ReportEvidence for CookPackageReport,
    "runtimeAutomation": EvidenceSet,
    "runtimeObservation": run-private ReportEvidence for RuntimeObservation
  },
  "finalPackageHashes": five PackageFamilyEvidence records,
  "builds": {
    "editor": BuildEvidence for CorsairsUEEditor,
    "game": BuildEvidence for CorsairsUE
  },
  "package": PackageEvidence,
  "issues": []
}
```

Every report/build evidence path except the fixed top/runtime/package-family
sources is below the matching transaction run/package root. Receipt `Launch`
is a pre-stage source identity, not an archive path. The orchestrator uses the
receipt target/product identity plus UAT outputs to synthesize strict sorted
stage/container and archive inventory JSON, then uses those inventories to map to
the one exact archived `.app/Contents/MacOS/<launch-leaf>`; recursive executable
discovery or accepting the example path is forbidden. With `-pak`, UFS files
need not be loose: the audit records the exact container list, extracts the
three named paths with the matching engine `UnrealPak` into the run-private
evidence root, and hashes those copies. The packaged runtime test independently
reads the same three virtual paths and must report the same hashes.
The orchestrator strictly parses each synthesized inventory/container-list/
cook report and `SandboxAttestation` before linking it. Package inventory,
container listing/extraction, and executable binding precede the probe, but no
PASS `CookPackageReport` or `PackageEvidence` is written until the probe has
produced and self-validated the sandbox attestation. Its validator reopens
the archived `Info.plist` and run-private canonical entitlement plist, requires
the `Info.plist` bundle identifier,
the entitlement `com.apple.security.app-sandbox=true`, the two container
metadata identifiers, and the enclosing archive app/executable relation to
agree exactly; it also rehashes the run-private entitlement plist and archived
`Info.plist`. The packaged C++ test emits exactly one
canonical `CORSAIRS_TERRAIN_RUNTIME_JSON=` automation event containing the
literal `RuntimeObservation` object; the orchestrator rejects zero, duplicate,
noncanonical, truncated, or mismatched events and atomically writes the parsed
object as the run-private runtime-observation report. Engine-generated
automation trees are retained and hashed as `EvidenceSet` inputs, but are not
misrepresented as Task 8-owned JSON DTOs.

`validate_base_bundle` reopens every listed file, recomputes every hash,
recomputes every package-family digest, requires exact transaction-root
containment, requires all manifest/report identities to agree, and requires the
checker's complete physical family evidence to equal import pass 2 and current
disk at publication. It strictly reparses the two inventory reports, every
container-list report, `CookPackageReport`, `SandboxAttestation`, and
`RuntimeObservation`; duplicated
package fields must be byte-for-byte equal after canonical parsing. It also
verifies the three source/staged/extracted/runtime hash identities and absence
of `CorsairsImport` from receipts, stage/archive manifests, containers, and
runtime module descriptors. Package/build target receipts must be the same
evidence record, every container has exactly one matching container-list
report, and every runtime file names one listed container/member/extraction.
The bundle is the only
terrain-base input accepted by scene parity Task 8; loose report discovery is
forbidden.

## RED tests

Add the canonical mutation matrices and these additional focused tests:

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
test_package_family_hashes_include_every_sidecar
test_sidecar_add_remove_or_byte_change_breaks_idempotence
test_idempotence_report_mutation_matrix
test_texture_sampling_contract_is_complete
test_module_graph_has_no_editor_runtime_cycle
test_clean_checkout_orders_installer_before_game_build_and_cook
test_dirty_or_wrong_head_checkout_fails_before_mutation
test_base_bundle_rehashes_every_input
test_inventory_cook_and_runtime_report_mutation_matrices
test_sandbox_attestation_mutation_matrix
test_signed_bundle_derives_one_verified_application_container
test_runtime_report_uses_owned_container_scratch_not_repository_path
test_runtime_report_copy_rehashes_complete_no_follow_tree_and_modes
test_sandbox_scratch_crash_matrix_recovers_only_exact_owned_tree
test_foreign_or_mismatched_sandbox_scratch_fails_closed
test_base_bundle_rejects_mutable_report_receipt_or_binary_path
test_prior_bundle_inputs_remain_valid_after_later_run_failure
test_base_bundle_publish_failure_preserves_previous_bytes
test_failure_after_each_unreal_step_restores_package_bytes
test_crash_after_each_transaction_seam_recovers_on_fresh_invocation
test_crash_after_bundle_replace_restores_previous_bundle_and_inputs
test_committed_crash_keeps_new_bundle_and_only_cleans_recovery
test_malformed_or_mismatched_recovery_fails_closed_with_exact_command
test_outer_recovery_resolves_publisher_before_snapshot_restore
test_outer_recovery_resolves_installer_before_snapshot_restore
test_failed_inner_recovery_preserves_outer_snapshot
test_crash_after_inner_retry_before_active_process_clear_recovers
test_timeout_reaps_only_owned_process_group_before_restore
test_success_removes_recovery_journal
test_preexisting_process_is_refused_and_never_killed
test_thermal_warning_or_pmset_failure_launches_no_heavy_command
test_all_heavy_commands_are_sequential_nice_and_capped_at_two
test_capture_is_delegated_to_scene_task8_not_faked
```

The manifest matrix deletes/changes every required field and covers each of
the seven path/hash entries, leaf-only/absolute/traversal/nested-runs/mixed-
run/symlink escape, missing/tampered file, all metrics and budgets, mask
content, texture-ID ordering, and recomputed total bytes. Every case asserts
the exact `code` and JSON-pointer-like `field`.

The level/import/check/base matrices start from literal complete fixtures for
the DTOs above, delete or mistype every key and nested field, add every unknown
key, mutate every canonical path/hash/size/family digest, reject unsorted or
duplicate records and invalid PASS/FAIL issue relations, and assert the exact
`code` plus JSON-pointer-like `field`. They prove that pass 2 cannot save equal
primary bytes while adding/removing/changing a sidecar and still call itself
zero-mutation. The base matrix also mutates every field in linked inventory,
container-list, cook-package, sandbox-attestation, and runtime-observation DTOs
(updating the outer
file hash when necessary) and still requires the nested strict parser to fail.
The separate in-memory sandbox-probe parser matrix deletes, mistypes, adds, and
mutates every `SandboxProbeEvent` field and proves that it cannot be linked as a
bundle/report evidence record.

Sandbox tests use literal complete plist/report fixtures and mutate every
attestation/probe field, signed bundle ID, sandbox entitlement, container
metadata identifier/creator, path containment, owner/mode/device/inode, and
marker byte. Tree tests include nested readable files, unreadable entries,
symlink/hard-link/device leaves, lexical-order traps, source mutation during
copy, destination mutation, and every named scratch failure/crash seam. They
require exact relative-path/mode/size/hash equality after the durable copy and
prove that pre-existing or foreign user-container content is never deleted.

Rollback tests inject both an ordinary failure and a no-cleanup simulated crash
at every named seam, start a fresh orchestrator for crash recovery, and compare
the complete prior snapshot plus prior bundle's full transitive validation.
They cover absent/present old top/runtime/bundle/package files, all allowed
sidecar suffixes, modes, failure during restore, malformed/foreign journal
paths, PID reuse/start-token mismatch, a live unrelated process, and both
pre- and post-`COMMITTED` outcomes. Process/thermal tests use injected process
tables and literal healthy/warning/nonzero `pmset -g therm` results; every
warning or probe failure launches zero heavy commands, every pre-existing
process remains alive, and the recorded command list proves one-at-a-time
ordering, `nice -n 10`, `-j2`/`-MaxParallelActions=2`, and serial `ctest -j1`.

Run the smallest RED commands serially:

```bash
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_module_dependencies.ModuleDependencyTests.test_module_graph_has_no_editor_runtime_cycle -v
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_reference_terrain_rules.ReferenceTerrainRulesTests.test_level_build_report_requires_corsairs_game_mode -v
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_reference_terrain_rules -v
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  scripts.tests.test_build_garner_reference_terrain -v
```

Expected initial RED is a missing production module/function or a literal
contract failure. A zero-test run, skip, hidden pre-existing asset, or stale
report is not RED evidence.

Add Editor automation tests:

```text
Corsairs.Terrain.ReferenceAssets.DefaultGameMode
Corsairs.Terrain.ReferenceAssets.ReferenceActor
```

They explicitly load `/Game/Maps/Garner`, require world object
`/Game/Maps/Garner.Garner`, soft-load the exact GameMode, marker, four assets,
sampling/material contract, one world-qualified actor, transform/bounds,
hidden overlaps, and no `MI_grass05`. They do not rely on startup map state.

Add a Development-only runtime test in `CorsairsGame`:

```text
Corsairs.Terrain.ReferenceRuntime.CookedWorld
```

It runs in the packaged Game target and uses runtime `LoadObject<UWorld>` to
load the cooked `/Game/Maps/Garner.Garner` package without starting play or a
network login. From the cooked `UWorld`/persistent level it requires the same
package/world/default-GameMode/reference-actor/asset/material-binding/transform
facts. Editor-only expression nodes are checked before cook by the Editor test;
the runtime test checks the cooked effective blend/shading/material bindings
and does not pretend stripped graph objects are runtime evidence.
It also opens the staged `Data/character_map.json`, `garner.block.raw`, and
`garner.terrain.json` through the production paths and records their hashes.
It includes no Editor API and no `CorsairsImport` dependency. Actual BeginPlay,
login, character and graphical runtime evidence remains the downstream scene
capture gate.

## GREEN commands

The final production GREEN run is executed only from the frozen source commit
created in **Source-freeze commit** below; that section is an execution
prerequisite despite appearing after the command listing for readability.
Dirty-tree RED/unit development may precede the freeze, but none of its
generated evidence is final acceptance.

Run one heavy command at a time. Before each heavyweight step the orchestrator
must prove no competing owned/pre-existing UE/client/build process and check
`pmset -g therm`; a warning aborts before launch rather than heating the Mac.
The thermal probe must exit zero and, after trimming whitespace, contain all
three literal healthy lines `No thermal warning level has been recorded`,
`No performance warning level has been recorded`, and
`No CPU power status has been recorded`. Any recorded warning/status, missing
healthy line, unrecognized nonempty diagnostic, or nonzero exit is fail-closed
and launches no heavyweight child.

```bash
nice -n 10 cmake -S tools/AssetConverter \
  -B tools/AssetConverter/build -DCMAKE_BUILD_TYPE=Debug
nice -n 10 cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests TerrainPageBudgetProbe -j2
nice -n 10 ctest --test-dir tools/AssetConverter/build \
  -j1 --output-on-failure
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest discover \
  -s CorsairsUE/Scripts/tests -v
PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 -m unittest \
  scripts.tests.test_build_garner_reference_terrain -v

PYTHONDONTWRITEBYTECODE=1 nice -n 10 python3 \
  scripts/build_garner_reference_terrain.py \
  --repo-root "$PWD" \
  --manifest artifacts/maps/garner.reference-albedo.json \
  --map /Game/Maps/Garner \
  --output artifacts/maps/reports
```

Inside that tracked orchestrator, build the Editor before the four Editor
commands. In the literal examples below `TXN_ID` and `SOURCE_HEAD` are the
already journaled 32-lowercase-hex transaction ID and clean 40-lowercase-hex
commit; no command writes to a shared fixed report leaf. Each Python entry
point receives those two final positional identity arguments, rejects a report
path outside `reports/runs/$TXN_ID`, and writes them unchanged into its DTO:

```bash
TXN_REPORT_ROOT="$PWD/artifacts/maps/reports/runs/$TXN_ID"
nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -WaitMutex -MaxParallelActions=2

UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/build_reference_terrain_level.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $TXN_REPORT_ROOT/reference-terrain-level-build.json $TXN_ID $SOURCE_HEAD" \
  -unattended -nop4 -NullRHI -NoSound
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $TXN_REPORT_ROOT/reference-terrain-import-pass1.json $TXN_ID $SOURCE_HEAD" \
  -unattended -nop4 -NullRHI -NoSound
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $TXN_REPORT_ROOT/reference-terrain-import-pass2.json $TXN_ID $SOURCE_HEAD" \
  -unattended -nop4 -NullRHI -NoSound
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/check_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $TXN_REPORT_ROOT/reference-terrain-level-build.json $TXN_REPORT_ROOT/reference-terrain-import-pass1.json $TXN_REPORT_ROOT/reference-terrain-import-pass2.json $TXN_REPORT_ROOT/reference-terrain-check.json $TXN_ID $SOURCE_HEAD" \
  -unattended -nop4 -NullRHI -NoSound
```

Then, within the same orchestrator, run Editor automation, the runtime target,
cook/package, and packaged runtime test sequentially:

```bash
UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Terrain.ReferenceAssets" \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$TXN_REPORT_ROOT/reference-terrain-editor-automation"

nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUE Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -WaitMutex -MaxParallelActions=2

nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/RunUAT.sh" \
  BuildCookRun \
  -project="$PWD/CorsairsUE/CorsairsUE.uproject" \
  -noP4 -unattended -utf8output \
  -platform=Mac -clientconfig=Development \
  -skipbuild -cook -stage -pak -package -archive \
  -map=/Game/Maps/Garner \
  -AdditionalCookerOptions=-SkipZenStore \
  -CookOutputDir="$PWD/artifacts/maps/package-run/$TXN_ID/cooked/Mac" \
  -stagingdirectory="$PWD/artifacts/maps/package-run/$TXN_ID/stage" \
  -archivedirectory="$PWD/artifacts/maps/package-run/$TXN_ID/archive" \
  -MaxParallelActions=2

# Bound by the orchestrator to the one receipt-selected executable from the
# validated archive inventory; this is not a recursive/example-path lookup.
PACKAGED_EXECUTABLE="<validated receipt-selected archive executable>"
nice -n 10 \
  "$PACKAGED_EXECUTABLE" \
  -unattended -NullRHI -NoSound -stdout -FullStdOutLogOutput \
  -CorsairsTerrainTransaction="$TXN_ID" \
  -CorsairsTerrainSourceHead="$SOURCE_HEAD" \
  -CorsairsTerrainSandboxProbe \
  -ExecCmds="Automation RunTests Corsairs.Terrain.ReferenceRuntime" \
  -TestExit="Automation Test Queue Empty"

# The orchestrator alone sets these from the validated probe event and exact
# durable journal; they are not environment/user inputs.
PROBED_AUTOMATION_REPORTS_ROOT="<validated event automationReportsRoot>"
SANDBOX_REPORT_ROOT="$PROBED_AUTOMATION_REPORTS_ROOT/CorsairsTerrainTask8-$TXN_ID/reference-terrain-runtime"
nice -n 10 \
  "$PACKAGED_EXECUTABLE" \
  -unattended -NullRHI -NoSound -stdout -FullStdOutLogOutput \
  -CorsairsTerrainTransaction="$TXN_ID" \
  -CorsairsTerrainSourceHead="$SOURCE_HEAD" \
  -ExecCmds="Automation RunTests Corsairs.Terrain.ReferenceRuntime" \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$SANDBOX_REPORT_ROOT"
```

After the second packaged process exits, the host performs the exact durable
no-follow copy/rehash protocol above into
`$TXN_REPORT_ROOT/reference-terrain-runtime`; no engine process writes directly
to that repository destination.

The UAT forwarding argument above is literal: the emitted Cook commandlet argv
contains exactly one `-SkipZenStore` and no `-ZenStore`. This keeps the
run-private cook on the filesystem and out of UE's default Zen storage path.
The literal `Mac` leaf also makes Cook and UAT Stage consume the same directory.
The `-package` step finalizes that staged data into the self-contained Mac app
before `-archive` copies the app into the run-private archive directory.

The real orchestrator allocates a unique `package-run/<transaction-id>` rather
than any shared directory. It treats the target receipt's `Launch` as the
pre-stage build product, copies the receipt and every project-owned named build
product to the transaction evidence root, and uses the UAT stage/archive
manifests for the exact archive mapping. It hashes those copies, the mapped
executable, every automation-report file, package container/list, runtime
report, and all extracted packaged runtime data. Cook success without the
explicit Garner map/assets/data in the container and packaged runtime is
failure.

After every heavy command and at final exit:

```bash
ps -axo pid,etime,%cpu,%mem,nice,command | \
  rg -i 'UnrealEditor|CorsairsUE\.app/Contents/MacOS/CorsairsUE|Game\.exe|CrossOver|wine|Build\.sh|RunUAT' || true
pmset -g therm
```

The process audit filters out its own `ps`/`rg` lines and requires no owned
Task 8 process. Never use a broad process-name kill.

## Downstream same-resolution capture gate

`garner-terrain-base.json` is not final visual acceptance. Scene parity Task 8
must validate and hash-link it, populate the real scene, then produce three
distinct files:

```text
original-223325-278475-1920x1080.png
ue-223325-278475-1920x1080.png
side-by-side-223325-278475-1920x1080.png
```

Both raw frames use the approved `(223325,278475)` position, 1920x1080 world
viewport, 16:9, 30 logical ticks/s, capture tick 120, target height 100 cm,
arm 6103.2778 cm, pitch -55.00798 deg, horizontal FOV 54.0222067 deg, yaw
90 deg, fixed exposure/tone settings, and exact process/DB/`system.ini`
rollback from the scene plan. Distinct hashes, runtime position evidence,
five landmark projection errors <=5 px, and human inspection remain
mandatory. Terrain Task 8 must not start either client or weaken/de-duplicate
that downstream gate.

The scene plan has a stricter publication bridge after this task: it may use
terrain import pass 1 as its seed evidence, but it still creates and validates
its own GameMode-configuration report followed by two additional zero-mutation
terrain import reports before scene staging. `garner-terrain-base.json` is
therefore an immutable input to, not a replacement for, the scene run's
`garner-base-bundle.json`; the two bundle names and report roles must not be
collapsed.

## Source-freeze commit (execute before final production GREEN)

After implementation and the smallest RED/GREEN suites, but before executing
the final production GREEN block above, run `git diff --check`, inspect the
exact staged path list, and commit only tracked Task 8 sources. Generated Content,
artifacts/package/reports/base bundle, installed runtime data, DBs, pycache,
and recovery journals remain ignored.

```bash
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
  CorsairsUE/Source/CorsairsImport/CorsairsImport.Build.cs \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsReferenceTerrainRuntimeTests.cpp \
  CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs \
  scripts/build_garner_reference_terrain.py \
  scripts/tests/test_build_garner_reference_terrain.py
git commit -m "feat(ue): import and attest Garner reference terrain"
```

Run the entire final GREEN block from that clean immutable HEAD. If any failure
requires a tracked source edit, all evidence from the prior candidate is
invalid: make a new reviewed source commit and rerun the complete block from
the new clean HEAD. On success, require empty
`git status --porcelain=v1 --untracked-files=all`, require the published bundle
and every Task 8 report to carry that exact HEAD, and make no post-evidence
tracked commit or amend. Thus the handed-off Task 8 commit is exactly the source
commit whose clean checkout produced the acceptance bundle.

## Dependency order

```text
Terrain Tasks 1..6 -> Terrain Task 7 manifest + installer
Movement Task 6 -> runtime module boundary and block-grid consumer
Terrain Task 7 + Movement Task 6 -> Terrain Task 8 owned chain
Terrain Task 8 base bundle + Scene Task 7 -> Scene Task 8 dual-client capture
```

---
