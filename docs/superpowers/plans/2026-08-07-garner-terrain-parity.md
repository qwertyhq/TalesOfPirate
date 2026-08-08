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


def validate_level_build_report(data: dict) -> list[ValidationIssue]: ...
def validate_import_report(data: dict) -> list[ValidationIssue]: ...
def validate_idempotent_import_reports(
    first: dict,
    second: dict,
) -> list[ValidationIssue]: ...
def validate_check_report(data: dict) -> list[ValidationIssue]: ...
def validate_base_bundle(data: dict, bundle_path: Path) -> list[ValidationIssue]: ...
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
The staged/archive audit also requires no `CorsairsImport` runtime binary or
module descriptor; its presence is an Editor-module leak even if cook exits
zero.

## Editor report contracts

All entry points accept exactly the positional arguments from the canonical
plan, write `<report>.tmp`, flush/fsync, `os.replace`, fsync the parent, then
self-validate. They write an issue-bearing failure report and raise
`RuntimeError` on any issue. Exit code or the Unreal line `Python script
executed successfully` is never accepted without a valid report.

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
  normalized map filename/hash, replacement flag, and `issues=[]`.

The builder's marker-only world is explicitly not playable and not visual
acceptance. It contains no fabricated scene, legacy terrain, PlayerStart, or
hidden ignored prerequisite.

`import_reference_terrain.py` validates before mutation, then:

1. Loads the exact map/world/marker/GameMode and refuses any mismatch.
2. Snapshots the five canonical package hashes: mesh, texture, base material,
   material instance, and Garner map.
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

Each import report uses the canonical schema: manifest/map/canonical object
identity, sorted object-level `created`/`updated`/`deleted`, sorted
`savedPackages`, five sorted `finalPackageHashes`, and `issues`. Pass 2 must
have all four mutation/save lists empty and byte-identical final hashes to
pass 1.

`check_reference_terrain.py` is read-only. It validates and hashes the
manifest, level-build report, both import reports and idempotence relation;
loads the exact world; independently inspects the actual marker, GameMode,
assets, texture sampling, material graph/usages, mesh/Nanite/material slots,
one actor/transform/bounds, all legacy overlaps/tags/visibility, and five
package hashes. It never creates, mutates, dirties, deletes, or saves a
package. Success requires actual package hashes exactly equal pass 2 and
zero issues/visible overlaps/grass overrides. Its observations also require
literal counts `materialFallbackCount=0`, `materialUsageErrorCount=0`, and
`translucentNaniteCount=0`; a WorldGrid/default/white fallback or a material
that merely compiles without both required usages is fatal.

## Durable rollback and base-bundle publication

`scripts/build_garner_reference_terrain.py` is the sole clean-checkout Task 8
orchestrator. Pure tests inject the command runner, file operations and
failpoints. Before the first generated-Content mutation it:

1. Acquires an exclusive ignored transaction lock and refuses any pre-existing
   `UnrealEditor`, `UnrealEditor-Cmd`, packaged client, `Game.exe`,
   CrossOver/Wine, or active Task 8 transaction; it never kills a pre-existing
   process.
2. Before rerunning Task 7 or installing anything, snapshots the exact previous
   bytes/existence/mode/hash of the current top terrain manifest, the two
   installed runtime files, the prior terrain-base bundle, and the five
   managed package families, including `.uasset`, `.umap`, `.uexp`, `.ubulk`,
   and `.uptnl` sidecars. A newly created Task 7 run directory may remain
   unreferenced after failure, but no prior top pointer or installed file may
   change.
3. Saves a 0600 journal plus same-volume recovery directory, fsyncs files and
   parent directories, and records the pre-run directory listing so a failed
   import cannot leak newly generated sidecars.
4. Runs and validates the Task 7 manifest+installer chain, then starts every
   later subprocess in a dedicated process group, records PID/PGID,
   waits in foreground, and on timeout/failure/interruption terminates/reaps
   only that owned group before rollback.

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
       -> packaged NullRHI runtime smoke
```

If any command, report, hash, save, automation, cook, or runtime gate fails,
the orchestrator first reaps its owned process tree, then restores all old
top-manifest/runtime-data/base-bundle/package-family bytes and modes or removes
files that were previously absent, removes every newly introduced managed-root
sidecar, verifies exact hashes/absence, and leaves the journal/recovery
material in place if verification fails. Success deletes recovery material
only after all reports and final files hash correctly.

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
five final package files
Mac Editor and Game build identities
cook/package report and packaged executable
packaged runtime smoke report
```

Publication uses a unique regular temp file in the same directory, flush,
file fsync, `os.replace`, and parent-directory fsync. A failed validation or
replace leaves the previous bundle byte-identical.

`validate_base_bundle` reopens every listed file, recomputes every hash,
requires all manifest/report identities to agree, and requires the checker's
actual hashes to equal import pass 2. The bundle is the only terrain-base
input accepted by scene parity Task 8; loose report discovery is forbidden.

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
test_idempotence_report_mutation_matrix
test_texture_sampling_contract_is_complete
test_module_graph_has_no_editor_runtime_cycle
test_clean_checkout_orders_installer_before_game_build_and_cook
test_base_bundle_rehashes_every_input
test_base_bundle_publish_failure_preserves_previous_bytes
test_failure_after_each_unreal_step_restores_package_bytes
test_timeout_reaps_only_owned_process_group_before_restore
test_success_removes_recovery_journal
test_capture_is_delegated_to_scene_task8_not_faked
```

The manifest matrix deletes/changes every required field and covers each of
the seven path/hash entries, leaf-only/absolute/traversal/nested-runs/mixed-
run/symlink escape, missing/tampered file, all metrics and budgets, mask
content, texture-ID ordering, and recomputed total bytes. Every case asserts
the exact `code` and JSON-pointer-like `field`.

The level/import/check/base matrices delete or mistype every field, mutate
every canonical path/hash, reject unsorted/duplicate records and nonempty
issues, and prove that pass 2 cannot save equal bytes and still call itself
zero-mutation. Rollback tests inject a failure after builder, each import,
checker, automation, build, cook, and runtime smoke.

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

Run one heavy command at a time. Before each heavyweight step the orchestrator
must prove no competing owned/pre-existing UE/client/build process and check
`pmset -g therm`; a warning aborts before launch rather than heating the Mac.

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
commands:

```bash
nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -WaitMutex -MaxParallelActions=2

UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/build_reference_terrain_level.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-level-build.json" \
  -unattended -nop4 -NullRHI -NoSound
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-import-pass1.json" \
  -unattended -nop4 -NullRHI -NoSound
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/import_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-import-pass2.json" \
  -unattended -nop4 -NullRHI -NoSound
nice -n 10 "$UE" "$PWD/CorsairsUE/CorsairsUE.uproject" -run=pythonscript \
  -script="$PWD/CorsairsUE/Scripts/check_reference_terrain.py $PWD/artifacts/maps/garner.reference-albedo.json /Game/Maps/Garner $PWD/artifacts/maps/reports/reference-terrain-level-build.json $PWD/artifacts/maps/reports/reference-terrain-import-pass1.json $PWD/artifacts/maps/reports/reference-terrain-import-pass2.json $PWD/artifacts/maps/reports/reference-terrain-check.json" \
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
  -ReportOutputPath="$PWD/artifacts/maps/reports/reference-terrain-editor-automation"

nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUE Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -WaitMutex -MaxParallelActions=2

nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/RunUAT.sh" \
  BuildCookRun \
  -project="$PWD/CorsairsUE/CorsairsUE.uproject" \
  -noP4 -unattended -utf8output \
  -platform=Mac -clientconfig=Development \
  -skipbuild -cook -stage -pak -archive \
  -map=/Game/Maps/Garner \
  -CookOutputDir="$PWD/artifacts/maps/package-run/cooked" \
  -stagingdirectory="$PWD/artifacts/maps/package-run/stage" \
  -archivedirectory="$PWD/artifacts/maps/package-run/archive" \
  -MaxParallelActions=2

nice -n 10 \
  "$PWD/artifacts/maps/package-run/archive/Mac/CorsairsUE.app/Contents/MacOS/CorsairsUE" \
  -unattended -NullRHI -NoSound -stdout -FullStdOutLogOutput \
  -ExecCmds="Automation RunTests Corsairs.Terrain.ReferenceRuntime" \
  -TestExit="Automation Test Queue Empty" \
  -ReportOutputPath="$PWD/artifacts/maps/reports/reference-terrain-runtime"
```

The real orchestrator allocates a unique `package-run/<transaction-id>` rather
than reusing the literal example directory above. It resolves the actual
archived executable path from the UAT receipt instead of silently accepting
the example path when UAT emits a different layout. It hashes the receipt,
executable, every automation-report file, package container/list, runtime
report, and staged runtime data. Cook success without the explicit Garner
map/assets/data in the package is failure.

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

## Commit

After fresh GREEN evidence, run `git diff --check`, inspect the exact staged
path list, and commit only tracked Task 8 sources. Generated Content,
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

## Dependency order

```text
Terrain Tasks 1..6 -> Terrain Task 7 manifest + installer
Movement Task 6 -> runtime module boundary and block-grid consumer
Terrain Task 7 + Movement Task 6 -> Terrain Task 8 owned chain
Terrain Task 8 base bundle + Scene Task 7 -> Scene Task 8 dual-client capture
```

---
