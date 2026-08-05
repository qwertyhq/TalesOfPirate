# AssetConverter: `.lgo` → glTF — план реализации (этапы 0–1)

> **Для агентов-исполнителей:** ОБЯЗАТЕЛЬНЫЙ СУБ-СКИЛЛ — используйте
> `superpowers:subagent-driven-development` (рекомендуется) или
> `superpowers:executing-plans` для выполнения задача-за-задачей. Шаги размечены
> чекбоксами (`- [ ]`).

**Цель:** кроссплатформенная консольная утилита, разбирающая проприетарный формат
геометрии `.lgo` движка MindPower3D и записывающая glTF 2.0, с пакетным прогоном
по всем 6702 файлам и отчётом об ошибках.

**Архитектура:** самостоятельный парсер на чистом C++23 без внешних зависимостей.
Движковые исходники (`sources/Engine/Asset/AssetLoaders.cpp`,
`Model/lwExpObj.h`, `Core/lwITypes.h`, `Core/lwITypes2.h`) — спецификация формата,
а не библиотека: они непереносимы (Windows/MSVC/DirectX), а разработка идёт на
macOS. Каждый блок файла разбирается отдельным парсером, который обязан прочитать
ровно объявленное в заголовке число байт — это основной инвариант тестирования.

**Стек:** C++23, CMake ≥ 3.20, Apple clang 21 (macOS ARM64) и MSVC (Windows x64).
Тесты — собственный минимальный harness, без сторонних фреймворков. glTF и JSON
пишутся собственным кодом.

## Глобальные ограничения

- **Стандарт:** C++23 (`CMAKE_CXX_STANDARD 23`, `CXX_STANDARD_REQUIRED ON`).
- **Никаких внешних зависимостей.** Ни сети при сборке, ни vcpkg, ни FetchContent.
  Только стандартная библиотека.
- **Namespace:** весь код — в `Corsairs::Tools::AssetConverter`. Закрывающая скобка
  комментируется: `} // namespace Corsairs::Tools::AssetConverter`.
- **Защита заголовков:** `#pragma once`, никаких include guard'ов.
- **Именование:** методы и свободные функции — `PascalCase`; поля класса —
  `_camelCase`; открытые поля struct — `PascalCase`; локальные переменные и
  параметры — `camelCase`; `enum class` с underlying-типом из `<cstdint>`,
  значения — `SCREAMING_CASE`.
- **Типы:** только фиксированной ширины из `<cstdint>` (`std::uint32_t` и т.п.).
  Никаких `DWORD`, `int`, `unsigned long`.
- **Фигурные скобки обязательны** даже для однострочного `if`.
- **Никаких `try`/`catch`** — ошибки возвращаются статусами. Это правило проекта
  из `CLAUDE.md`.
- **Никаких C-строковых функций** — `std::string` / `std::string_view` / `std::format`.
- **Порядок байт:** файлы little-endian (записаны на x86). Целевые платформы
  (x86-64, ARM64) тоже little-endian, поэтому `std::memcpy` в POD корректен.
  Дополнительная перестановка байт не нужна и не делается.
- **Коммиты:** без трейлеров, без упоминаний AI/Claude (правило пользователя).
- **Сборка и тесты выполняются из** `tools/AssetConverter/build/`.

## Спецификация формата (проверена на реальном файле)

`Client/model/character/0066000000.lgo`, 18 044 байта:

```
смещение  размер  содержимое
0x000     4       version: 0x0000 | 0x1000 | 0x1001 | 0x1002 | 0x1003 | 0x1004 | 0x1005
0x004     4       Id
0x008     4       ParentId
0x00C     4       Type
0x010     64      MatLocal (4x4 float, row-major)
0x050     16      Rcci: CtrlId, DeclId, VsId, PsId
0x060     8       StateCtrl: 8 x uint8
0x068     4       MtlSize
0x06C     4       MeshSize
0x070     4       HelperSize
0x074     4       AnimSize
0x078     ...     блоки в порядке: материалы, геометрия, helper'ы, анимация
```

Заголовок после version — ровно **116** байт. Для проверочного файла:
`4 + 116 + 1008 + 16824 + 92 + 0 = 18044` — совпадает с размером файла.

Размеры фиксированных структур (проверяются `static_assert`):

| Структура | Байт | Состав |
|---|---|---|
| `ColorValue4f` | 16 | 4 × float |
| `Material` | 68 | Dif, Amb, Spe, Emi (по 16) + Power (4) |
| `RenderStateAtom` | 12 | State, Value0, Value1 |
| `TexInfo` | 208 | 11 × uint32 (44) + FileName[64] + ReservedData (4) + TssSet[8] (96) |
| `MtlTexInfo` | 1004 | Opacity (4) + TranspType (4) + Material (68) + RsSet[8] (96) + TexSeq[4] (832) |
| `MeshInfoHeader` | 128 | 8 × uint32 (32) + RsSet[8] (96) |
| `SubsetInfo` | 16 | PrimitiveNum, StartIndex, VertexNum, MinIndex |
| `BlendInfo` | 20 | Index[4] (4 байта) + Weight[4] (16) |
| `GeomObjHeader` | 116 | см. раскладку выше |

Блок материалов: `uint32 MtlNum`, затем `MtlNum` × `MtlTexInfo`.
Проверка: `4 + 1004 * MtlNum == MtlSize`.

Блок геометрии для `version >= 0x1004`:

```
MeshInfoHeader (128 байт): Fvf, PtType, VertexNum, IndexNum, SubsetNum,
                           BoneIndexNum, BoneInflFactor, VertexElementNum,
                           RsSet[8]
если VertexElementNum > 0 : VertexElement[VertexElementNum]  (по 8 байт)
если VertexNum > 0        : Vertex[VertexNum]                (float3, 12 байт)
если Fvf & 0x0010         : Normal[VertexNum]                (float3, 12 байт)
если Fvf & 0x0100         : Texcoord0[VertexNum]             (float2, 8 байт)
иначе если Fvf & 0x0200   : Texcoord0, Texcoord1
иначе если Fvf & 0x0300   : Texcoord0, Texcoord1, Texcoord2
иначе если Fvf & 0x0400   : Texcoord0..Texcoord3
если Fvf & 0x0040         : VertexColor[VertexNum]           (uint32 ARGB)
если BoneIndexNum > 0     : Blend[VertexNum] (20 б), BoneIndex[BoneIndexNum] (4 б)
если IndexNum > 0         : Index[IndexNum]                  (uint32)
если SubsetNum > 0        : Subset[SubsetNum]                (16 байт)
```

Константы FVF (из DirectX 9, значения фиксированы):
`D3DFVF_NORMAL = 0x0010`, `D3DFVF_DIFFUSE = 0x0040`, `D3DFVF_TEX1 = 0x0100`,
`D3DFVF_TEX2 = 0x0200`, `D3DFVF_TEX3 = 0x0300`, `D3DFVF_TEX4 = 0x0400`.

**Версии ниже `0x1004` в этом плане не поддерживаются** — они получают статус
`VERSION_UNSUPPORTED` и попадают в отчёт. Решение о поддержке принимается по
результатам пакетного прогона: если старых файлов единицы, они конвертируются
вручную; если их много — это отдельная задача с известным объёмом.

## Имена текстур

Внутри `.lgo` имена текстур записаны с расширением `.BMP` (проверено:
`0066000000.lgo` ссылается на `0066000000.BMP`), тогда как на диске лежит
`Client/texture/character/0066000000.png`. Игра когда-то перешла с BMP на PNG, не
переписывая модели, — движок ищет файл, игнорируя расширение.

Следствие для любого кода, который будет резолвить текстуры: искать по имени
**без расширения**, перебирая фактически существующие (`.png`, `.dds`, `.jpg`).
В этом плане резолвинг не выполняется — материалы в glTF не пишутся, — но
`LgoMaterial::TextureName` возвращает имя как есть, из файла, без нормализации.
Нормализация — задача будущего манифеста материалов.

## Преобразование системы координат

MindPower3D — DirectX 9, левосторонняя система координат. glTF 2.0 —
правосторонняя. Преобразование при записи:

- Позиции и нормали: `Z → -Z`.
- Порядок обхода треугольника: индексы `(i0, i1, i2)` → `(i0, i2, i1)`.

Обе операции обязательны вместе. Только отрицание Z вывернет модели наизнанку;
только смена winding'а отзеркалит их.

## Структура файлов

```
tools/AssetConverter/
  CMakeLists.txt                  сборка библиотеки, CLI и тестов
  include/Corsairs/Tools/AssetConverter/
    BinaryReader.h                чтение POD из буфера с проверкой границ
    LgoTypes.h                    POD-структуры формата + static_assert размеров
    LgoParser.h                   разбор .lgo, статусы, диагностика
    JsonWriter.h                  минимальный генератор JSON
    GltfWriter.h                  запись glTF 2.0 (.gltf + .bin)
    ConversionReport.h            накопление статистики пакетного прогона
  src/
    LgoParser.cpp
    JsonWriter.cpp
    GltfWriter.cpp
    ConversionReport.cpp
    Main.cpp                      CLI
  tests/
    TestHarness.h                 минимальный тест-фреймворк
    TestMain.cpp                  точка входа тестов
    TestBinaryReader.cpp
    TestLgoTypes.cpp
    TestLgoParser.cpp
    TestJsonWriter.cpp
    TestGltfWriter.cpp
```

`BinaryReader.h` и `LgoTypes.h` — header-only: первый состоит из коротких inline-
методов, второй — только из POD-объявлений и `static_assert`.

Тестовые данные не копируются в репозиторий: тесты читают реальные ассеты из
`Client/model/`. Путь к корню репозитория передаётся тестам через определение
компилятора `CORSAIRS_REPO_ROOT`, задаваемое в CMake.

---

## Task 1: каркас сборки, BinaryReader и тест-harness

**Файлы:**
- Создать: `tools/AssetConverter/CMakeLists.txt`
- Создать: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/BinaryReader.h`
- Создать: `tools/AssetConverter/tests/TestHarness.h`
- Создать: `tools/AssetConverter/tests/TestMain.cpp`
- Создать: `tools/AssetConverter/tests/TestBinaryReader.cpp`

**Интерфейсы:**
- Предоставляет: класс `BinaryReader` с методами `Offset()`, `Size()`,
  `Remaining()`, `CanRead(std::size_t)`, `Read<T>(T&)`, `ReadArray<T>(T*, std::size_t)`,
  `Skip(std::size_t)`, `Seek(std::size_t)`; свободную функцию
  `std::optional<std::vector<std::uint8_t>> ReadWholeFile(const std::filesystem::path&)`.
- Предоставляет: макросы `CORSAIRS_TEST(name)`, `REQUIRE(cond)`,
  `REQUIRE_EQ(actual, expected)` и функцию `RunAllTests()` из `TestHarness.h`.

- [ ] **Шаг 1: Написать падающий тест**

Создать `tools/AssetConverter/tests/TestBinaryReader.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include "TestHarness.h"

#include <cstdint>
#include <vector>

namespace {

using Corsairs::Tools::AssetConverter::BinaryReader;

CORSAIRS_TEST(BinaryReader_ReadsUint32LittleEndian) {
    const std::vector<std::uint8_t> data{0x04, 0x10, 0x00, 0x00};
    BinaryReader reader{data};

    std::uint32_t value = 0;
    REQUIRE(reader.Read(value));
    REQUIRE_EQ(value, 0x1004u);
    REQUIRE_EQ(reader.Offset(), 4u);
    REQUIRE_EQ(reader.Remaining(), 0u);
}

CORSAIRS_TEST(BinaryReader_RefusesReadPastEnd) {
    const std::vector<std::uint8_t> data{0x01, 0x02};
    BinaryReader reader{data};

    std::uint32_t value = 0;
    REQUIRE(!reader.Read(value));
    REQUIRE_EQ(reader.Offset(), 0u);
}

CORSAIRS_TEST(BinaryReader_ReadArrayAdvancesByElementCount) {
    const std::vector<std::uint8_t> data{
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
    };
    BinaryReader reader{data};

    std::uint32_t values[3]{};
    REQUIRE(reader.ReadArray(values, 3));
    REQUIRE_EQ(values[0], 1u);
    REQUIRE_EQ(values[2], 3u);
    REQUIRE_EQ(reader.Offset(), 12u);
}

CORSAIRS_TEST(BinaryReader_SkipAndSeek) {
    const std::vector<std::uint8_t> data(16, 0);
    BinaryReader reader{data};

    REQUIRE(reader.Skip(8));
    REQUIRE_EQ(reader.Offset(), 8u);
    REQUIRE(!reader.Skip(9));
    REQUIRE_EQ(reader.Offset(), 8u);
    REQUIRE(reader.Seek(2));
    REQUIRE_EQ(reader.Offset(), 2u);
    REQUIRE(!reader.Seek(17));
}

} // namespace
```

- [ ] **Шаг 2: Запустить тест и убедиться, что он не собирается**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

Ожидается: FAIL — `CMakeLists.txt` не существует.

- [ ] **Шаг 3: Написать тест-harness**

Создать `tools/AssetConverter/tests/TestHarness.h`:

```cpp
#pragma once

#include <cstddef>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter::Testing {

struct TestCase {
    std::string_view Name;
    void (*Body)(bool&);
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(std::string_view name, void (*body)(bool&)) {
        Registry().push_back(TestCase{name, body});
    }
};

// Возвращает число провалившихся тестов; 0 — успех.
inline int RunAllTests() {
    std::size_t failed = 0;
    for (const TestCase& test : Registry()) {
        bool ok = true;
        test.Body(ok);
        if (ok) {
            std::cout << std::format("  PASS  {}\n", test.Name);
        }
        else {
            std::cout << std::format("  FAIL  {}\n", test.Name);
            ++failed;
        }
    }
    std::cout << std::format("\n{} тестов, провалено {}\n", Registry().size(), failed);
    return static_cast<int>(failed);
}

} // namespace Corsairs::Tools::AssetConverter::Testing

#define CORSAIRS_TEST(name)                                                        \
    static void name(bool& corsairsTestOk);                                        \
    static const ::Corsairs::Tools::AssetConverter::Testing::Registrar              \
        name##_registrar{#name, &name};                                            \
    static void name(bool& corsairsTestOk)

#define REQUIRE(cond)                                                              \
    do {                                                                           \
        if (!(cond)) {                                                             \
            corsairsTestOk = false;                                                \
            std::cout << std::format("        {}:{} REQUIRE({}) не выполнено\n",   \
                                     __FILE__, __LINE__, #cond);                   \
            return;                                                                \
        }                                                                          \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                               \
    do {                                                                           \
        const auto corsairsActual = (actual);                                      \
        const auto corsairsExpected = (expected);                                  \
        if (!(corsairsActual == corsairsExpected)) {                               \
            corsairsTestOk = false;                                                \
            std::cout << std::format("        {}:{} {} == {}: получено {}, ожидалось {}\n", \
                                     __FILE__, __LINE__, #actual, #expected,       \
                                     corsairsActual, corsairsExpected);            \
            return;                                                                \
        }                                                                          \
    } while (false)
```

Создать `tools/AssetConverter/tests/TestMain.cpp`:

```cpp
#include "TestHarness.h"

int main() {
    return Corsairs::Tools::AssetConverter::Testing::RunAllTests();
}
```

- [ ] **Шаг 4: Написать BinaryReader**

Создать `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/BinaryReader.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

// Последовательное чтение POD-значений из буфера с проверкой границ. Любая
// попытка прочитать за пределами буфера возвращает false и НЕ сдвигает курсор,
// поэтому вызывающий код может корректно сообщить об усечённом файле.
//
// Файлы MindPower3D записаны на x86 (little-endian); целевые платформы тоже
// little-endian, поэтому memcpy в POD корректен без перестановки байт.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> data)
        : _data(data) {
    }

    [[nodiscard]] std::size_t Offset() const {
        return _offset;
    }

    [[nodiscard]] std::size_t Size() const {
        return _data.size();
    }

    [[nodiscard]] std::size_t Remaining() const {
        return _data.size() - _offset;
    }

    [[nodiscard]] bool CanRead(std::size_t bytes) const {
        return Remaining() >= bytes;
    }

    template <typename T>
    [[nodiscard]] bool Read(T& out) {
        static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::Read требует POD-тип");
        if (!CanRead(sizeof(T))) {
            return false;
        }
        std::memcpy(&out, _data.data() + _offset, sizeof(T));
        _offset += sizeof(T);
        return true;
    }

    template <typename T>
    [[nodiscard]] bool ReadArray(T* out, std::size_t count) {
        static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::ReadArray требует POD-тип");
        if (count == 0) {
            return true;
        }
        const std::size_t bytes = sizeof(T) * count;
        if (bytes / sizeof(T) != count) {
            return false;
        }
        if (!CanRead(bytes)) {
            return false;
        }
        std::memcpy(out, _data.data() + _offset, bytes);
        _offset += bytes;
        return true;
    }

    [[nodiscard]] bool Skip(std::size_t bytes) {
        if (!CanRead(bytes)) {
            return false;
        }
        _offset += bytes;
        return true;
    }

    [[nodiscard]] bool Seek(std::size_t offset) {
        if (offset > _data.size()) {
            return false;
        }
        _offset = offset;
        return true;
    }

private:
    std::span<const std::uint8_t> _data;
    std::size_t _offset{0};
};

// Читает файл целиком. std::nullopt — файл не открылся или не читается.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> ReadWholeFile(
    const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        return std::nullopt;
    }

    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (size > 0 && !stream.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return std::nullopt;
    }
    return buffer;
}

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 5: Написать CMakeLists.txt**

Создать `tools/AssetConverter/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(CorsairsAssetConverter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Корень репозитория — тесты читают реальные ассеты из Client/model.
get_filename_component(CORSAIRS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(AssetConverterLib STATIC
    src/LgoParser.cpp
    src/JsonWriter.cpp
    src/GltfWriter.cpp
    src/ConversionReport.cpp
)
target_include_directories(AssetConverterLib PUBLIC include)

if(MSVC)
    target_compile_options(AssetConverterLib PRIVATE /W4 /permissive-)
else()
    target_compile_options(AssetConverterLib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(AssetConverter src/Main.cpp)
target_link_libraries(AssetConverter PRIVATE AssetConverterLib)

enable_testing()

add_executable(AssetConverterTests
    tests/TestMain.cpp
    tests/TestBinaryReader.cpp
    tests/TestLgoTypes.cpp
    tests/TestLgoParser.cpp
    tests/TestJsonWriter.cpp
    tests/TestGltfWriter.cpp
)
target_link_libraries(AssetConverterTests PRIVATE AssetConverterLib)
target_include_directories(AssetConverterTests PRIVATE tests)
target_compile_definitions(AssetConverterTests
    PRIVATE CORSAIRS_REPO_ROOT="${CORSAIRS_REPO_ROOT}")

add_test(NAME AssetConverterTests COMMAND AssetConverterTests)
```

CMakeLists перечисляет файлы, которые появятся в следующих задачах. Чтобы этот
шаг собрался прямо сейчас, создайте их как заглушки с единственной строкой —
пустым namespace'ом. Для `.cpp` в `src/`:

```cpp
namespace Corsairs::Tools::AssetConverter {
} // namespace Corsairs::Tools::AssetConverter
```

Для тестовых файлов `tests/TestLgoTypes.cpp`, `tests/TestLgoParser.cpp`,
`tests/TestJsonWriter.cpp`, `tests/TestGltfWriter.cpp` — то же содержимое.
`src/Main.cpp` временно:

```cpp
int main() {
    return 0;
}
```

- [ ] **Шаг 6: Собрать и запустить тесты**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/AssetConverterTests
```

Ожидается: 4 теста, провалено 0.

- [ ] **Шаг 7: Коммит**

```bash
cd /Users/ivan/code/TalesOfPirate
cat >> .gitignore <<'EOF'
tools/AssetConverter/build/
EOF
git add tools/AssetConverter .gitignore
git commit -m "AssetConverter: каркас сборки, BinaryReader и тест-harness"
```

---

## Task 2: POD-структуры формата с проверкой размеров

**Файлы:**
- Создать: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/LgoTypes.h`
- Изменить: `tools/AssetConverter/tests/TestLgoTypes.cpp` (заменить заглушку)

**Интерфейсы:**
- Потребляет: ничего.
- Предоставляет: POD-структуры `ColorValue4f`, `Material`, `RenderStateAtom`,
  `TexInfo`, `MtlTexInfo`, `MeshInfoHeader`, `SubsetInfo`, `BlendInfo`,
  `GeomObjHeader`; константы `kGeomObjHeaderSize`, `kMtlTexInfoSize`,
  `kMeshInfoHeaderSize`; `enum class FvfFlag : std::uint32_t`;
  функцию `IsKnownVersion(std::uint32_t)`.

- [ ] **Шаг 1: Написать падающий тест**

Заменить содержимое `tools/AssetConverter/tests/TestLgoTypes.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/LgoTypes.h"

#include "TestHarness.h"

#include <cstdint>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

CORSAIRS_TEST(LgoTypes_StructSizesMatchOnDiskLayout) {
    REQUIRE_EQ(sizeof(AC::ColorValue4f), 16u);
    REQUIRE_EQ(sizeof(AC::Material), 68u);
    REQUIRE_EQ(sizeof(AC::RenderStateAtom), 12u);
    REQUIRE_EQ(sizeof(AC::TexInfo), 208u);
    REQUIRE_EQ(sizeof(AC::MtlTexInfo), 1004u);
    REQUIRE_EQ(sizeof(AC::MeshInfoHeader), 128u);
    REQUIRE_EQ(sizeof(AC::SubsetInfo), 16u);
    REQUIRE_EQ(sizeof(AC::BlendInfo), 20u);
    REQUIRE_EQ(sizeof(AC::GeomObjHeader), 116u);
}

CORSAIRS_TEST(LgoTypes_NamedConstantsMatchSizeof) {
    REQUIRE_EQ(AC::kGeomObjHeaderSize, sizeof(AC::GeomObjHeader));
    REQUIRE_EQ(AC::kMtlTexInfoSize, sizeof(AC::MtlTexInfo));
    REQUIRE_EQ(AC::kMeshInfoHeaderSize, sizeof(AC::MeshInfoHeader));
}

CORSAIRS_TEST(LgoTypes_KnownVersions) {
    REQUIRE(AC::IsKnownVersion(0x0000u));
    REQUIRE(AC::IsKnownVersion(0x1000u));
    REQUIRE(AC::IsKnownVersion(0x1004u));
    REQUIRE(AC::IsKnownVersion(0x1005u));
    REQUIRE(!AC::IsKnownVersion(0x1006u));
    REQUIRE(!AC::IsKnownVersion(0xDEADBEEFu));
}

} // namespace
```

- [ ] **Шаг 2: Запустить тест и убедиться, что он падает**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build
```

Ожидается: FAIL — `LgoTypes.h` не найден.

- [ ] **Шаг 3: Написать LgoTypes.h**

Создать `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/LgoTypes.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace Corsairs::Tools::AssetConverter {

inline constexpr std::size_t kMaxName = 64;
inline constexpr std::size_t kMaxTextureStageNum = 4;
inline constexpr std::size_t kTexTssNum = 8;
inline constexpr std::size_t kMtlRsNum = 8;
inline constexpr std::size_t kMeshRsNum = 8;
inline constexpr std::size_t kObjectStateNum = 8;

// Флаги FVF DirectX 9. Значения фиксированы спецификацией D3D9 и продублированы
// здесь, чтобы не тянуть Windows SDK.
enum class FvfFlag : std::uint32_t {
    NORMAL  = 0x0010,
    DIFFUSE = 0x0040,
    TEX1    = 0x0100,
    TEX2    = 0x0200,
    TEX3    = 0x0300,
    TEX4    = 0x0400,
};

[[nodiscard]] inline bool HasFvf(std::uint32_t fvf, FvfFlag flag) {
    const std::uint32_t bits = static_cast<std::uint32_t>(flag);
    return (fvf & bits) == bits;
}

// Версии .lgo, встречающиеся в датасете. Источник — LgoLoader::IsKnownVersion
// в sources/Engine/Asset/AssetLoaders.cpp.
[[nodiscard]] inline bool IsKnownVersion(std::uint32_t version) {
    return version == 0x0000u || (version >= 0x1000u && version <= 0x1005u);
}

// Минимальная версия, чья раскладка блока геометрии реализована в этом плане.
inline constexpr std::uint32_t kMinSupportedVersion = 0x1004u;

#pragma pack(push, 1)

struct Vector2 {
    float X;
    float Y;
};

struct Vector3 {
    float X;
    float Y;
    float Z;
};

struct ColorValue4f {
    float R;
    float G;
    float B;
    float A;
};

struct Material {
    ColorValue4f Dif;
    ColorValue4f Amb;
    ColorValue4f Spe;
    ColorValue4f Emi;
    float Power;
};

struct RenderStateAtom {
    std::uint32_t State;
    std::uint32_t Value0;
    std::uint32_t Value1;
};

struct TexInfo {
    std::uint32_t Stage;
    std::uint32_t Level;
    std::uint32_t Usage;
    std::uint32_t Format;
    std::uint32_t Pool;
    std::uint32_t ByteAlignmentFlag;
    std::uint32_t Type;
    std::uint32_t Width;
    std::uint32_t Height;
    std::uint32_t ColorkeyType;
    std::uint32_t Colorkey;
    char FileName[kMaxName];
    // В движке здесь был `void* data`, менявший размер между x86 и x64 и ломавший
    // разбор файлов. Заменён на 4-байтный плейсхолдер, фиксирующий формат.
    std::uint32_t ReservedData;
    RenderStateAtom TssSet[kTexTssNum];
};

struct MtlTexInfo {
    float Opacity;
    std::uint32_t TranspType;
    Material Mtl;
    RenderStateAtom RsSet[kMtlRsNum];
    TexInfo TexSeq[kMaxTextureStageNum];
};

struct MeshInfoHeader {
    std::uint32_t Fvf;
    std::uint32_t PtType;
    std::uint32_t VertexNum;
    std::uint32_t IndexNum;
    std::uint32_t SubsetNum;
    std::uint32_t BoneIndexNum;
    std::uint32_t BoneInflFactor;
    std::uint32_t VertexElementNum;
    RenderStateAtom RsSet[kMeshRsNum];
};

struct SubsetInfo {
    std::uint32_t PrimitiveNum;
    std::uint32_t StartIndex;
    std::uint32_t VertexNum;
    std::uint32_t MinIndex;
};

struct BlendInfo {
    std::uint8_t Index[4];
    float Weight[4];
};

struct RenderCtrlCreateInfo {
    std::uint32_t CtrlId;
    std::uint32_t DeclId;
    std::uint32_t VsId;
    std::uint32_t PsId;
};

struct GeomObjHeader {
    std::uint32_t Id;
    std::uint32_t ParentId;
    std::uint32_t Type;
    float MatLocal[16];
    RenderCtrlCreateInfo Rcci;
    std::uint8_t StateCtrl[kObjectStateNum];
    std::uint32_t MtlSize;
    std::uint32_t MeshSize;
    std::uint32_t HelperSize;
    std::uint32_t AnimSize;
};

// Элемент вершинной декларации D3D9 (D3DVERTEXELEMENT9) — 8 байт.
struct VertexElement {
    std::uint16_t Stream;
    std::uint16_t Offset;
    std::uint8_t Type;
    std::uint8_t Method;
    std::uint8_t Usage;
    std::uint8_t UsageIndex;
};

#pragma pack(pop)

inline constexpr std::size_t kGeomObjHeaderSize = sizeof(GeomObjHeader);
inline constexpr std::size_t kMtlTexInfoSize = sizeof(MtlTexInfo);
inline constexpr std::size_t kMeshInfoHeaderSize = sizeof(MeshInfoHeader);

// Раскладка на диске зафиксирована файлами, записанными десятилетия назад.
// Любое расхождение — ошибка компиляции, а не тихо испорченные данные.
static_assert(sizeof(ColorValue4f) == 16, "ColorValue4f: раскладка на диске 16 байт");
static_assert(sizeof(Material) == 68, "Material: раскладка на диске 68 байт");
static_assert(sizeof(RenderStateAtom) == 12, "RenderStateAtom: раскладка на диске 12 байт");
static_assert(sizeof(TexInfo) == 208, "TexInfo: раскладка на диске 208 байт");
static_assert(sizeof(MtlTexInfo) == 1004, "MtlTexInfo: раскладка на диске 1004 байта");
static_assert(sizeof(MeshInfoHeader) == 128, "MeshInfoHeader: раскладка на диске 128 байт");
static_assert(sizeof(SubsetInfo) == 16, "SubsetInfo: раскладка на диске 16 байт");
static_assert(sizeof(BlendInfo) == 20, "BlendInfo: раскладка на диске 20 байт");
static_assert(sizeof(GeomObjHeader) == 116, "GeomObjHeader: раскладка на диске 116 байт");
static_assert(sizeof(VertexElement) == 8, "VertexElement: раскладка на диске 8 байт");
static_assert(sizeof(Vector2) == 8, "Vector2: раскладка на диске 8 байт");
static_assert(sizeof(Vector3) == 12, "Vector3: раскладка на диске 12 байт");

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 4: Собрать и запустить тесты**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: 7 тестов, провалено 0.

- [ ] **Шаг 5: Коммит**

```bash
cd /Users/ivan/code/TalesOfPirate
git add tools/AssetConverter
git commit -m "AssetConverter: POD-структуры формата .lgo с проверкой размеров"
```

---

## Task 3: разбор заголовка и блока материалов

**Файлы:**
- Создать: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/LgoParser.h`
- Изменить: `tools/AssetConverter/src/LgoParser.cpp` (заменить заглушку)
- Изменить: `tools/AssetConverter/tests/TestLgoParser.cpp` (заменить заглушку)

**Интерфейсы:**
- Потребляет: `BinaryReader`, `ReadWholeFile`, все типы из `LgoTypes.h`.
- Предоставляет: `enum class LgoStatus : std::uint32_t`;
  `std::string_view ToString(LgoStatus)`;
  `struct LgoDiagnostics { LgoStatus Status; std::string Detail; std::uint32_t Version; }`;
  `struct LgoMaterial { MtlTexInfo Raw; std::string TextureName(std::size_t stage) const; }`;
  `struct LgoGeomObj { std::uint32_t Version; GeomObjHeader Header; std::vector<LgoMaterial> Materials; LgoMesh Mesh; }`;
  `std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t>, LgoDiagnostics&)`.
  Структура `LgoMesh` объявляется в этой задаче целиком (со всеми векторами
  атрибутов), но `ParseLgo` пока оставляет её пустой — блок геометрии
  пропускается по объявленному размеру. Наполняет её Task 4.

- [ ] **Шаг 1: Написать падающий тест**

Заменить содержимое `tools/AssetConverter/tests/TestLgoParser.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path SampleLgoPath() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "model" / "character" / "0066000000.lgo";
}

CORSAIRS_TEST(LgoParser_ParsesHeaderOfRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());
    REQUIRE_EQ(bytes->size(), 18044u);

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::OK));

    REQUIRE_EQ(obj->Version, 0x1004u);
    REQUIRE_EQ(obj->Header.Id, 0u);
    REQUIRE_EQ(obj->Header.ParentId, 0xFFFFFFFFu);
    REQUIRE_EQ(obj->Header.MtlSize, 1008u);
    REQUIRE_EQ(obj->Header.MeshSize, 16824u);
    REQUIRE_EQ(obj->Header.HelperSize, 92u);
    REQUIRE_EQ(obj->Header.AnimSize, 0u);
}

CORSAIRS_TEST(LgoParser_HeaderMatLocalIsIdentityForSample) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    REQUIRE_EQ(obj->Header.MatLocal[0], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[5], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[10], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[15], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[1], 0.0f);
}

CORSAIRS_TEST(LgoParser_ParsesMaterialBlockAndConsumesExactSize) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    // MtlSize 1008 = 4 (MtlNum) + 1004 * 1
    REQUIRE_EQ(obj->Materials.size(), 1u);
    // Проверено на реальном файле: имя текстуры лежит по смещению 0x154
    // и равно "0066000000.BMP". Расширение намеренно НЕ проверяется — см.
    // раздел «Имена текстур» ниже.
    REQUIRE_EQ(obj->Materials[0].TextureName(0), std::string{"0066000000.BMP"});
}

CORSAIRS_TEST(LgoParser_RejectsTruncatedVersion) {
    const std::vector<std::uint8_t> bytes{0x04, 0x10};

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(bytes, diag);
    REQUIRE(!obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::VERSION_TRUNCATED));
}

CORSAIRS_TEST(LgoParser_RejectsUnknownVersion) {
    std::vector<std::uint8_t> bytes(200, 0);
    bytes[0] = 0xEF;
    bytes[1] = 0xBE;
    bytes[2] = 0xAD;
    bytes[3] = 0xDE;

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(bytes, diag);
    REQUIRE(!obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::VERSION_UNKNOWN));
}

CORSAIRS_TEST(LgoParser_RejectsBlockSizesLargerThanFile) {
    std::vector<std::uint8_t> bytes(4 + AC::kGeomObjHeaderSize, 0);
    bytes[0] = 0x04;
    bytes[1] = 0x10;

    // MtlSize по смещению 4 + 100 = 104 (поле идёт после Id/ParentId/Type/
    // MatLocal[64]/Rcci[16]/StateCtrl[8] = 4+4+4+64+16+8 = 100).
    bytes[4 + 100] = 0xFF;
    bytes[4 + 101] = 0xFF;

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(bytes, diag);
    REQUIRE(!obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::BLOCK_SIZES_INCONSISTENT));
}

} // namespace
```

- [ ] **Шаг 2: Запустить тест и убедиться, что он падает**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build
```

Ожидается: FAIL — `LgoParser.h` не найден.

- [ ] **Шаг 3: Написать LgoParser.h**

Создать `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/LgoParser.h`:

```cpp
#pragma once

#include "Corsairs/Tools/AssetConverter/LgoTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

// Причина отказа ParseLgo. OK — успех; OK_WITH_TRAILING_DATA — данные валидны,
// но в файле остался хвост за границей объявленных блоков (движок его тоже
// игнорирует, поэтому это предупреждение, а не ошибка).
enum class LgoStatus : std::uint32_t {
    OK = 0,
    OK_WITH_TRAILING_DATA,
    VERSION_TRUNCATED,
    VERSION_UNKNOWN,
    VERSION_UNSUPPORTED,
    HEADER_TRUNCATED,
    BLOCK_SIZES_INCONSISTENT,
    MTL_BLOCK_MALFORMED,
    MESH_BLOCK_MALFORMED,
};

[[nodiscard]] std::string_view ToString(LgoStatus status);

struct LgoDiagnostics {
    LgoStatus Status{LgoStatus::OK};
    std::string Detail;
    std::uint32_t Version{0};
};

struct LgoMaterial {
    MtlTexInfo Raw{};

    // Имя файла текстуры для указанной стадии; пустая строка — стадия не задана.
    // Поле FileName на диске может не иметь завершающего нуля, поэтому длина
    // ограничивается явно.
    [[nodiscard]] std::string TextureName(std::size_t stage) const;
};

struct LgoMesh {
    MeshInfoHeader Header{};
    std::vector<Vector3> Positions;
    std::vector<Vector3> Normals;
    std::vector<Vector2> Texcoords[kMaxTextureStageNum];
    std::vector<std::uint32_t> VertexColors;
    std::vector<BlendInfo> Blends;
    std::vector<std::uint32_t> BoneIndices;
    std::vector<std::uint32_t> Indices;
    std::vector<SubsetInfo> Subsets;
    std::vector<VertexElement> VertexElements;
};

struct LgoGeomObj {
    std::uint32_t Version{0};
    GeomObjHeader Header{};
    std::vector<LgoMaterial> Materials;
    LgoMesh Mesh;
};

// Разбирает .lgo целиком. std::nullopt — файл непригоден; причина в diag.
// Блоки helper и anim пропускаются по объявленному размеру: они не нужны для
// статической геометрии, но их размеры участвуют в проверке целостности.
[[nodiscard]] std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t> bytes,
                                                 LgoDiagnostics& diag);

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 4: Написать LgoParser.cpp — заголовок и материалы**

Заменить содержимое `tools/AssetConverter/src/LgoParser.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <cstring>
#include <format>

namespace Corsairs::Tools::AssetConverter {

std::string_view ToString(LgoStatus status) {
    switch (status) {
    case LgoStatus::OK:                       return "OK";
    case LgoStatus::OK_WITH_TRAILING_DATA:    return "OK_WITH_TRAILING_DATA";
    case LgoStatus::VERSION_TRUNCATED:        return "VERSION_TRUNCATED";
    case LgoStatus::VERSION_UNKNOWN:          return "VERSION_UNKNOWN";
    case LgoStatus::VERSION_UNSUPPORTED:      return "VERSION_UNSUPPORTED";
    case LgoStatus::HEADER_TRUNCATED:         return "HEADER_TRUNCATED";
    case LgoStatus::BLOCK_SIZES_INCONSISTENT: return "BLOCK_SIZES_INCONSISTENT";
    case LgoStatus::MTL_BLOCK_MALFORMED:      return "MTL_BLOCK_MALFORMED";
    case LgoStatus::MESH_BLOCK_MALFORMED:     return "MESH_BLOCK_MALFORMED";
    }
    return "UNKNOWN";
}

std::string LgoMaterial::TextureName(std::size_t stage) const {
    if (stage >= kMaxTextureStageNum) {
        return {};
    }
    // Поле на диске может не иметь завершающего нуля, поэтому длина ищется
    // вручную с явным пределом. strnlen не используется: он объявлен в разных
    // заголовках на разных платформах.
    const char* name = Raw.TexSeq[stage].FileName;
    std::size_t length = 0;
    while (length < kMaxName && name[length] != '\0') {
        ++length;
    }
    return std::string{name, length};
}

namespace {

// Разбирает блок материалов и проверяет, что потрачено ровно mtlSize байт.
bool ParseMaterialBlock(BinaryReader& reader, std::uint32_t mtlSize,
                        std::vector<LgoMaterial>& out, LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    std::uint32_t mtlNum = 0;
    if (!reader.Read(mtlNum)) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MtlNum";
        return false;
    }

    const std::size_t expected = 4 + kMtlTexInfoSize * mtlNum;
    if (expected != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "MtlNum={} даёт {} байт, заголовок объявил MtlSize={}",
            mtlNum, expected, mtlSize);
        return false;
    }

    out.resize(mtlNum);
    for (std::uint32_t i = 0; i < mtlNum; ++i) {
        if (!reader.Read(out[i].Raw)) {
            diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан MtlTexInfo[{}]", i);
            return false;
        }
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format("прочитано {} байт, объявлено {}", consumed, mtlSize);
        return false;
    }
    return true;
}

} // namespace

std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t> bytes,
                                   LgoDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    LgoGeomObj obj;

    if (!reader.Read(obj.Version)) {
        diag.Status = LgoStatus::VERSION_TRUNCATED;
        diag.Detail = "файл короче 4 байт";
        return std::nullopt;
    }
    diag.Version = obj.Version;

    if (!IsKnownVersion(obj.Version)) {
        diag.Status = LgoStatus::VERSION_UNKNOWN;
        diag.Detail = std::format("version=0x{:08X}, ожидалось 0x0000 или 0x1000..0x1005",
                                  obj.Version);
        return std::nullopt;
    }

    if (obj.Version < kMinSupportedVersion) {
        diag.Status = LgoStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format("version=0x{:08X} ниже поддерживаемой 0x{:08X}",
                                  obj.Version, kMinSupportedVersion);
        return std::nullopt;
    }

    if (!reader.Read(obj.Header)) {
        diag.Status = LgoStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("нужно {} байт заголовка, доступно {}",
                                  kGeomObjHeaderSize, reader.Remaining());
        return std::nullopt;
    }

    const std::uint64_t blocksSum =
        static_cast<std::uint64_t>(obj.Header.MtlSize) +
        static_cast<std::uint64_t>(obj.Header.MeshSize) +
        static_cast<std::uint64_t>(obj.Header.HelperSize) +
        static_cast<std::uint64_t>(obj.Header.AnimSize);
    const std::uint64_t expectedTotal = blocksSum + 4 + kGeomObjHeaderSize;

    if (expectedTotal > bytes.size()) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = std::format(
            "mtl={} mesh={} helper={} anim={}; ожидается {} байт, файл {} байт",
            obj.Header.MtlSize, obj.Header.MeshSize, obj.Header.HelperSize,
            obj.Header.AnimSize, expectedTotal, bytes.size());
        return std::nullopt;
    }

    if (obj.Header.MtlSize > 0) {
        if (!ParseMaterialBlock(reader, obj.Header.MtlSize, obj.Materials, diag)) {
            return std::nullopt;
        }
    }

    // Блок геометрии наполняется в Task 4; пока пропускается по объявленному
    // размеру, чтобы проверка целостности файла работала уже сейчас.
    if (obj.Header.MeshSize > 0 && !reader.Skip(obj.Header.MeshSize)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "блок геометрии выходит за границы файла";
        return std::nullopt;
    }

    if (obj.Header.HelperSize > 0 && !reader.Skip(obj.Header.HelperSize)) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = "блок helper выходит за границы файла";
        return std::nullopt;
    }

    if (obj.Header.AnimSize > 0 && !reader.Skip(obj.Header.AnimSize)) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = "блок анимации выходит за границы файла";
        return std::nullopt;
    }

    if (expectedTotal < bytes.size()) {
        diag.Status = LgoStatus::OK_WITH_TRAILING_DATA;
        diag.Detail = std::format("трейлер {} байт", bytes.size() - expectedTotal);
        return obj;
    }

    diag.Status = LgoStatus::OK;
    return obj;
}

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 5: Собрать и запустить тесты**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: 13 тестов, провалено 0.

- [ ] **Шаг 6: Коммит**

```bash
cd /Users/ivan/code/TalesOfPirate
git add tools/AssetConverter
git commit -m "AssetConverter: разбор заголовка и блока материалов .lgo"
```

---

## Task 4: разбор блока геометрии

**Файлы:**
- Изменить: `tools/AssetConverter/src/LgoParser.cpp` (добавить `ParseMeshBlock`,
  заменить `Skip` на вызов парсера)
- Изменить: `tools/AssetConverter/tests/TestLgoParser.cpp` (добавить тесты)

**Интерфейсы:**
- Потребляет: `LgoMesh` из Task 3, `BinaryReader`, типы из `LgoTypes.h`.
- Предоставляет: заполненное поле `LgoGeomObj::Mesh` после `ParseLgo`.

- [ ] **Шаг 1: Написать падающие тесты**

Дописать в `tools/AssetConverter/tests/TestLgoParser.cpp` перед закрывающим
`} // namespace`:

```cpp
CORSAIRS_TEST(LgoParser_ParsesMeshBlockOfRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    REQUIRE(mesh.Header.VertexNum > 0u);
    REQUIRE(mesh.Header.IndexNum > 0u);
    REQUIRE(mesh.Header.SubsetNum > 0u);

    REQUIRE_EQ(mesh.Positions.size(), static_cast<std::size_t>(mesh.Header.VertexNum));
    REQUIRE_EQ(mesh.Indices.size(), static_cast<std::size_t>(mesh.Header.IndexNum));
    REQUIRE_EQ(mesh.Subsets.size(), static_cast<std::size_t>(mesh.Header.SubsetNum));
}

CORSAIRS_TEST(LgoParser_MeshNormalsPresentWhenFvfSaysSo) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    if (AC::HasFvf(mesh.Header.Fvf, AC::FvfFlag::NORMAL)) {
        REQUIRE_EQ(mesh.Normals.size(), static_cast<std::size_t>(mesh.Header.VertexNum));
    }
    else {
        REQUIRE(mesh.Normals.empty());
    }
}

CORSAIRS_TEST(LgoParser_MeshIndicesStayInVertexRange) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    for (std::uint32_t index : mesh.Indices) {
        REQUIRE(index < mesh.Header.VertexNum);
    }
}

CORSAIRS_TEST(LgoParser_SubsetsCoverIndexBuffer) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    for (const AC::SubsetInfo& subset : mesh.Subsets) {
        const std::uint64_t last =
            static_cast<std::uint64_t>(subset.StartIndex) +
            static_cast<std::uint64_t>(subset.PrimitiveNum) * 3ull;
        REQUIRE(last <= static_cast<std::uint64_t>(mesh.Header.IndexNum));
    }
}
```

- [ ] **Шаг 2: Запустить тесты и убедиться, что они падают**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: FAIL — `Positions.size()` равен 0, а `VertexNum` больше нуля.

- [ ] **Шаг 3: Написать ParseMeshBlock**

В `tools/AssetConverter/src/LgoParser.cpp` добавить в анонимный namespace, после
`ParseMaterialBlock`:

```cpp
// Читает vector<T> длиной count. false — данные за границей буфера.
template <typename T>
bool ReadVector(BinaryReader& reader, std::vector<T>& out, std::uint32_t count) {
    if (count == 0) {
        return true;
    }
    out.resize(count);
    return reader.ReadArray(out.data(), count);
}

// Разбирает блок геометрии для version >= 0x1004 и проверяет, что потрачено
// ровно meshSize байт. Порядок массивов задан LgoLoader::LoadMeshInfo
// (sources/Engine/Asset/AssetLoaders.cpp) и обязателен к соблюдению.
bool ParseMeshBlock(BinaryReader& reader, std::uint32_t meshSize, LgoMesh& mesh,
                    LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    if (!reader.Read(mesh.Header)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MeshInfoHeader";
        return false;
    }

    const std::uint32_t vertexNum = mesh.Header.VertexNum;

    if (!ReadVector(reader, mesh.VertexElements, mesh.Header.VertexElementNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан VertexElements";
        return false;
    }

    if (!ReadVector(reader, mesh.Positions, vertexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны позиции вершин";
        return false;
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::NORMAL)) {
        if (!ReadVector(reader, mesh.Normals, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны нормали";
            return false;
        }
    }

    // Порядок проверок повторяет цепочку if/else if движка: TEX1 проверяется
    // первым, поэтому набор с четырьмя UV имеет флаг TEX4 и не совпадает с TEX1.
    std::uint32_t texcoordSets = 0;
    if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX1)) {
        texcoordSets = 1;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX2)) {
        texcoordSets = 2;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX3)) {
        texcoordSets = 3;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX4)) {
        texcoordSets = 4;
    }

    for (std::uint32_t set = 0; set < texcoordSets; ++set) {
        if (!ReadVector(reader, mesh.Texcoords[set], vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан UV-набор {}", set);
            return false;
        }
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::DIFFUSE)) {
        if (!ReadVector(reader, mesh.VertexColors, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны цвета вершин";
            return false;
        }
    }

    if (mesh.Header.BoneIndexNum > 0) {
        if (!ReadVector(reader, mesh.Blends, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны веса скиннинга";
            return false;
        }
        if (!ReadVector(reader, mesh.BoneIndices, mesh.Header.BoneIndexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны индексы костей";
            return false;
        }
    }

    if (!ReadVector(reader, mesh.Indices, mesh.Header.IndexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан индексный буфер";
        return false;
    }

    if (!ReadVector(reader, mesh.Subsets, mesh.Header.SubsetNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны подсеты";
        return false;
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != meshSize) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "прочитано {} байт, заголовок объявил MeshSize={} (fvf=0x{:08X}, "
            "vertexNum={}, indexNum={}, subsetNum={}, boneIndexNum={})",
            consumed, meshSize, mesh.Header.Fvf, mesh.Header.VertexNum,
            mesh.Header.IndexNum, mesh.Header.SubsetNum, mesh.Header.BoneIndexNum);
        return false;
    }
    return true;
}
```

- [ ] **Шаг 4: Подключить парсер геометрии**

В `ParseLgo` заменить блок пропуска геометрии:

```cpp
    // Блок геометрии наполняется в Task 4; пока пропускается по объявленному
    // размеру, чтобы проверка целостности файла работала уже сейчас.
    if (obj.Header.MeshSize > 0 && !reader.Skip(obj.Header.MeshSize)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "блок геометрии выходит за границы файла";
        return std::nullopt;
    }
```

на:

```cpp
    if (obj.Header.MeshSize > 0) {
        if (!ParseMeshBlock(reader, obj.Header.MeshSize, obj.Mesh, diag)) {
            return std::nullopt;
        }
    }
```

- [ ] **Шаг 5: Собрать и запустить тесты**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: 17 тестов, провалено 0.

Если `LgoParser_ParsesMeshBlockOfRealFile` падает с `MESH_BLOCK_MALFORMED` и
сообщением о несовпадении числа байт — раскладка FVF понята неверно. Сообщение
содержит `fvf`, `vertexNum`, `indexNum`, `subsetNum`, `boneIndexNum` и фактически
прочитанное число байт: разница делится на `vertexNum` и даёт размер
недочитанного или лишнего атрибута. Сверяйтесь с `LgoLoader::LoadMeshInfo`
(`sources/Engine/Asset/AssetLoaders.cpp:427`).

- [ ] **Шаг 6: Коммит**

```bash
cd /Users/ivan/code/TalesOfPirate
git add tools/AssetConverter
git commit -m "AssetConverter: разбор блока геометрии .lgo"
```

---

## Task 5: запись glTF 2.0

**Файлы:**
- Создать: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/JsonWriter.h`
- Изменить: `tools/AssetConverter/src/JsonWriter.cpp` (заменить заглушку)
- Создать: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/GltfWriter.h`
- Изменить: `tools/AssetConverter/src/GltfWriter.cpp` (заменить заглушку)
- Изменить: `tools/AssetConverter/tests/TestJsonWriter.cpp` (заменить заглушку)
- Изменить: `tools/AssetConverter/tests/TestGltfWriter.cpp` (заменить заглушку)

**Интерфейсы:**
- Потребляет: `LgoGeomObj`, `LgoMesh`, `SubsetInfo`, `Vector2`, `Vector3`.
- Предоставляет: класс `JsonWriter` с методами `BeginObject()`, `EndObject()`,
  `BeginArray()`, `EndArray()`, `Key(std::string_view)`, `Value(std::string_view)`,
  `Value(double)`, `Value(std::int64_t)`, `Value(bool)`, `Str()`.
- Предоставляет: `enum class GltfStatus : std::uint32_t { OK, EMPTY_MESH, WRITE_FAILED }`;
  `GltfStatus WriteGltf(const LgoGeomObj&, const std::filesystem::path& gltfPath, std::string& detail)`.

- [ ] **Шаг 1: Написать падающий тест для JsonWriter**

Заменить содержимое `tools/AssetConverter/tests/TestJsonWriter.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include "TestHarness.h"

#include <string>

namespace {

using Corsairs::Tools::AssetConverter::JsonWriter;

CORSAIRS_TEST(JsonWriter_EmitsFlatObject) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("version");
    writer.Value("2.0");
    writer.Key("count");
    writer.Value(static_cast<std::int64_t>(3));
    writer.EndObject();

    REQUIRE_EQ(writer.Str(), std::string{R"({"version":"2.0","count":3})"});
}

CORSAIRS_TEST(JsonWriter_EmitsNestedArrays) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("items");
    writer.BeginArray();
    writer.BeginObject();
    writer.Key("a");
    writer.Value(static_cast<std::int64_t>(1));
    writer.EndObject();
    writer.BeginObject();
    writer.Key("a");
    writer.Value(static_cast<std::int64_t>(2));
    writer.EndObject();
    writer.EndArray();
    writer.EndObject();

    REQUIRE_EQ(writer.Str(), std::string{R"({"items":[{"a":1},{"a":2}]})"});
}

CORSAIRS_TEST(JsonWriter_EscapesStrings) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("path");
    writer.Value(R"(a\b"c)");
    writer.EndObject();

    REQUIRE_EQ(writer.Str(), std::string{R"({"path":"a\\b\"c"})"});
}

CORSAIRS_TEST(JsonWriter_WritesFiniteDoublesWithoutExponentLoss) {
    JsonWriter writer;
    writer.BeginArray();
    writer.Value(0.5);
    writer.Value(-1.25);
    writer.EndArray();

    REQUIRE_EQ(writer.Str(), std::string{"[0.5,-1.25]"});
}

} // namespace
```

- [ ] **Шаг 2: Запустить и убедиться, что не собирается**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build
```

Ожидается: FAIL — `JsonWriter.h` не найден.

- [ ] **Шаг 3: Написать JsonWriter**

Создать `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/JsonWriter.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Corsairs::Tools::AssetConverter {

// Минимальный потоковый генератор JSON. Достаточен для glTF: сам расставляет
// запятые между элементами и экранирует строки. Не проверяет корректность
// вложенности — вызывающий код обязан парно закрывать объекты и массивы.
class JsonWriter {
public:
    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();

    void Key(std::string_view key);

    void Value(std::string_view value);
    void Value(double value);
    void Value(std::int64_t value);
    void Value(bool value);

    [[nodiscard]] const std::string& Str() const {
        return _out;
    }

private:
    void Separate();
    void WriteEscaped(std::string_view value);

    std::string _out;
    bool _needComma{false};
};

} // namespace Corsairs::Tools::AssetConverter
```

Заменить содержимое `tools/AssetConverter/src/JsonWriter.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

void JsonWriter::Separate() {
    if (_needComma) {
        _out += ',';
    }
    _needComma = true;
}

void JsonWriter::WriteEscaped(std::string_view value) {
    _out += '"';
    for (char c : value) {
        switch (c) {
        case '"':  _out += "\\\""; break;
        case '\\': _out += "\\\\"; break;
        case '\n': _out += "\\n";  break;
        case '\r': _out += "\\r";  break;
        case '\t': _out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                _out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            }
            else {
                _out += c;
            }
        }
    }
    _out += '"';
}

void JsonWriter::BeginObject() {
    Separate();
    _out += '{';
    _needComma = false;
}

void JsonWriter::EndObject() {
    _out += '}';
    _needComma = true;
}

void JsonWriter::BeginArray() {
    Separate();
    _out += '[';
    _needComma = false;
}

void JsonWriter::EndArray() {
    _out += ']';
    _needComma = true;
}

void JsonWriter::Key(std::string_view key) {
    Separate();
    WriteEscaped(key);
    _out += ':';
    _needComma = false;
}

void JsonWriter::Value(std::string_view value) {
    Separate();
    WriteEscaped(value);
}

void JsonWriter::Value(double value) {
    Separate();
    // {} для double даёт кратчайшее представление, восстанавливающее исходное
    // значение при обратном разборе, и не переходит в экспоненциальную форму
    // без необходимости.
    _out += std::format("{}", value);
}

void JsonWriter::Value(std::int64_t value) {
    Separate();
    _out += std::format("{}", value);
}

void JsonWriter::Value(bool value) {
    Separate();
    _out += value ? "true" : "false";
}

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 4: Собрать и убедиться, что тесты JsonWriter проходят**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: 21 тест, провалено 0.

- [ ] **Шаг 5: Написать падающий тест для GltfWriter**

Заменить содержимое `tools/AssetConverter/tests/TestGltfWriter.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path SampleLgo() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "model" / "character" / "0066000000.lgo";
}

std::filesystem::path OutputDir() {
    return std::filesystem::temp_directory_path() / "corsairs-gltf-tests";
}

CORSAIRS_TEST(GltfWriter_WritesGltfAndBinForRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "sample.gltf";
    const std::filesystem::path binPath = OutputDir() / "sample.bin";
    std::filesystem::remove(gltfPath);
    std::filesystem::remove(binPath);

    std::string detail;
    const AC::GltfStatus status = AC::WriteGltf(*obj, gltfPath, detail);
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    REQUIRE(std::filesystem::exists(gltfPath));
    REQUIRE(std::filesystem::exists(binPath));
    REQUIRE(std::filesystem::file_size(binPath) > 0u);
}

CORSAIRS_TEST(GltfWriter_GltfDeclaresVersionAndMeshCounts) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "sample2.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("version":"2.0")") != std::string::npos);
    REQUIRE(text.find(R"("POSITION")") != std::string::npos);
    REQUIRE(text.find(R"("meshes")") != std::string::npos);
    // Каждому подсету соответствует своя примитива.
    REQUIRE(text.find(R"("primitives")") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_RejectsMeshWithoutVertices) {
    AC::LgoGeomObj empty;
    empty.Version = 0x1004u;

    std::filesystem::create_directories(OutputDir());
    std::string detail;
    const AC::GltfStatus status =
        AC::WriteGltf(empty, OutputDir() / "empty.gltf", detail);
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::GltfStatus::EMPTY_MESH));
}

} // namespace
```

- [ ] **Шаг 6: Написать GltfWriter**

Создать `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/GltfWriter.h`:

```cpp
#pragma once

#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Corsairs::Tools::AssetConverter {

enum class GltfStatus : std::uint32_t {
    OK = 0,
    EMPTY_MESH,
    WRITE_FAILED,
};

// Пишет gltfPath и парный .bin рядом (то же имя, расширение .bin).
// detail заполняется человекочитаемой причиной при неуспехе.
//
// Преобразование системы координат: MindPower3D левосторонняя, glTF —
// правосторонняя, поэтому Z инвертируется, а порядок обхода треугольника
// меняется на противоположный. Обе операции обязательны вместе.
[[nodiscard]] GltfStatus WriteGltf(const LgoGeomObj& obj,
                                   const std::filesystem::path& gltfPath,
                                   std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
```

Заменить содержимое `tools/AssetConverter/src/GltfWriter.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"

#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::int64_t kComponentTypeFloat = 5126;
constexpr std::int64_t kComponentTypeUnsignedInt = 5125;
constexpr std::int64_t kTargetArrayBuffer = 34962;
constexpr std::int64_t kTargetElementArrayBuffer = 34963;
constexpr std::int64_t kModeTriangles = 4;

struct BufferView {
    std::size_t ByteOffset;
    std::size_t ByteLength;
    std::int64_t Target;
};

// Дописывает данные в буфер с выравниванием на 4 байта, как требует glTF 2.0.
BufferView AppendToBuffer(std::vector<std::uint8_t>& buffer, const void* data,
                          std::size_t bytes, std::int64_t target) {
    while (buffer.size() % 4 != 0) {
        buffer.push_back(0);
    }
    const std::size_t offset = buffer.size();
    buffer.resize(offset + bytes);
    if (bytes > 0) {
        std::memcpy(buffer.data() + offset, data, bytes);
    }
    return BufferView{offset, bytes, target};
}

void WriteAccessorBounds(JsonWriter& json, const std::vector<Vector3>& values) {
    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float minZ = minX;
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = maxX;
    float maxZ = maxX;

    for (const Vector3& v : values) {
        minX = v.X < minX ? v.X : minX;
        minY = v.Y < minY ? v.Y : minY;
        minZ = v.Z < minZ ? v.Z : minZ;
        maxX = v.X > maxX ? v.X : maxX;
        maxY = v.Y > maxY ? v.Y : maxY;
        maxZ = v.Z > maxZ ? v.Z : maxZ;
    }

    json.Key("min");
    json.BeginArray();
    json.Value(static_cast<double>(minX));
    json.Value(static_cast<double>(minY));
    json.Value(static_cast<double>(minZ));
    json.EndArray();

    json.Key("max");
    json.BeginArray();
    json.Value(static_cast<double>(maxX));
    json.Value(static_cast<double>(maxY));
    json.Value(static_cast<double>(maxZ));
    json.EndArray();
}

bool WriteFile(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    if (bytes > 0) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    return static_cast<bool>(stream);
}

} // namespace

GltfStatus WriteGltf(const LgoGeomObj& obj, const std::filesystem::path& gltfPath,
                     std::string& detail) {
    const LgoMesh& mesh = obj.Mesh;

    if (mesh.Positions.empty() || mesh.Indices.empty() || mesh.Subsets.empty()) {
        detail = "меш не содержит вершин, индексов или подсетов";
        return GltfStatus::EMPTY_MESH;
    }

    // Преобразование левосторонней системы координат в правостороннюю.
    std::vector<Vector3> positions = mesh.Positions;
    for (Vector3& p : positions) {
        p.Z = -p.Z;
    }

    std::vector<Vector3> normals = mesh.Normals;
    for (Vector3& n : normals) {
        n.Z = -n.Z;
    }

    // Смена порядка обхода треугольника — парная операция к инверсии Z.
    std::vector<std::uint32_t> indices = mesh.Indices;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t tmp = indices[i + 1];
        indices[i + 1] = indices[i + 2];
        indices[i + 2] = tmp;
    }

    std::vector<std::uint8_t> buffer;
    const BufferView positionView = AppendToBuffer(
        buffer, positions.data(), positions.size() * sizeof(Vector3), kTargetArrayBuffer);

    BufferView normalView{0, 0, kTargetArrayBuffer};
    const bool hasNormals = !normals.empty();
    if (hasNormals) {
        normalView = AppendToBuffer(buffer, normals.data(),
                                    normals.size() * sizeof(Vector3), kTargetArrayBuffer);
    }

    BufferView uvView{0, 0, kTargetArrayBuffer};
    const bool hasUv = !mesh.Texcoords[0].empty();
    if (hasUv) {
        uvView = AppendToBuffer(buffer, mesh.Texcoords[0].data(),
                                mesh.Texcoords[0].size() * sizeof(Vector2),
                                kTargetArrayBuffer);
    }

    const BufferView indexView = AppendToBuffer(
        buffer, indices.data(), indices.size() * sizeof(std::uint32_t),
        kTargetElementArrayBuffer);

    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");

    if (!WriteFile(binPath, buffer.data(), buffer.size())) {
        detail = "не удалось записать .bin";
        return GltfStatus::WRITE_FAILED;
    }

    // Собираем список bufferView в том же порядке, в каком они попадут в JSON.
    std::vector<BufferView> views;
    views.push_back(positionView);
    const std::int64_t positionViewIndex = 0;

    std::int64_t normalViewIndex = -1;
    if (hasNormals) {
        normalViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(normalView);
    }

    std::int64_t uvViewIndex = -1;
    if (hasUv) {
        uvViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(uvView);
    }

    const std::int64_t indexViewIndex = static_cast<std::int64_t>(views.size());
    views.push_back(indexView);

    JsonWriter json;
    json.BeginObject();

    json.Key("asset");
    json.BeginObject();
    json.Key("version");
    json.Value("2.0");
    json.Key("generator");
    json.Value("Corsairs AssetConverter");
    json.EndObject();

    json.Key("buffers");
    json.BeginArray();
    json.BeginObject();
    json.Key("uri");
    json.Value(binPath.filename().string());
    json.Key("byteLength");
    json.Value(static_cast<std::int64_t>(buffer.size()));
    json.EndObject();
    json.EndArray();

    json.Key("bufferViews");
    json.BeginArray();
    for (const BufferView& view : views) {
        json.BeginObject();
        json.Key("buffer");
        json.Value(static_cast<std::int64_t>(0));
        json.Key("byteOffset");
        json.Value(static_cast<std::int64_t>(view.ByteOffset));
        json.Key("byteLength");
        json.Value(static_cast<std::int64_t>(view.ByteLength));
        json.Key("target");
        json.Value(view.Target);
        json.EndObject();
    }
    json.EndArray();

    // Аккессоры в том же порядке: POSITION, [NORMAL], [TEXCOORD_0], индексы.
    json.Key("accessors");
    json.BeginArray();

    json.BeginObject();
    json.Key("bufferView");
    json.Value(positionViewIndex);
    json.Key("componentType");
    json.Value(kComponentTypeFloat);
    json.Key("count");
    json.Value(static_cast<std::int64_t>(positions.size()));
    json.Key("type");
    json.Value("VEC3");
    WriteAccessorBounds(json, positions);
    json.EndObject();

    std::int64_t normalAccessor = -1;
    if (hasNormals) {
        normalAccessor = 1;
        json.BeginObject();
        json.Key("bufferView");
        json.Value(normalViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(normals.size()));
        json.Key("type");
        json.Value("VEC3");
        json.EndObject();
    }

    std::int64_t uvAccessor = -1;
    if (hasUv) {
        uvAccessor = hasNormals ? 2 : 1;
        json.BeginObject();
        json.Key("bufferView");
        json.Value(uvViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(mesh.Texcoords[0].size()));
        json.Key("type");
        json.Value("VEC2");
        json.EndObject();
    }

    // По аккессору индексов на каждый подсет: они делят один bufferView,
    // отличаясь byteOffset и count.
    const std::int64_t firstSubsetAccessor =
        1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0);

    for (const SubsetInfo& subset : mesh.Subsets) {
        json.BeginObject();
        json.Key("bufferView");
        json.Value(indexViewIndex);
        json.Key("byteOffset");
        json.Value(static_cast<std::int64_t>(subset.StartIndex) *
                   static_cast<std::int64_t>(sizeof(std::uint32_t)));
        json.Key("componentType");
        json.Value(kComponentTypeUnsignedInt);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(subset.PrimitiveNum) * 3);
        json.Key("type");
        json.Value("SCALAR");
        json.EndObject();
    }
    json.EndArray();

    json.Key("meshes");
    json.BeginArray();
    json.BeginObject();
    json.Key("primitives");
    json.BeginArray();
    for (std::size_t i = 0; i < mesh.Subsets.size(); ++i) {
        json.BeginObject();
        json.Key("attributes");
        json.BeginObject();
        json.Key("POSITION");
        json.Value(static_cast<std::int64_t>(0));
        if (hasNormals) {
            json.Key("NORMAL");
            json.Value(normalAccessor);
        }
        if (hasUv) {
            json.Key("TEXCOORD_0");
            json.Value(uvAccessor);
        }
        json.EndObject();
        json.Key("indices");
        json.Value(firstSubsetAccessor + static_cast<std::int64_t>(i));
        json.Key("mode");
        json.Value(kModeTriangles);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    json.Key("nodes");
    json.BeginArray();
    json.BeginObject();
    json.Key("mesh");
    json.Value(static_cast<std::int64_t>(0));
    json.EndObject();
    json.EndArray();

    json.Key("scenes");
    json.BeginArray();
    json.BeginObject();
    json.Key("nodes");
    json.BeginArray();
    json.Value(static_cast<std::int64_t>(0));
    json.EndArray();
    json.EndObject();
    json.EndArray();

    json.Key("scene");
    json.Value(static_cast<std::int64_t>(0));

    json.EndObject();

    const std::string& text = json.Str();
    if (!WriteFile(gltfPath, text.data(), text.size())) {
        detail = "не удалось записать .gltf";
        return GltfStatus::WRITE_FAILED;
    }

    detail.clear();
    return GltfStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 7: Собрать и запустить тесты**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: 24 теста, провалено 0.

- [ ] **Шаг 8: Проверить результат сторонним валидатором**

Открыть `$TMPDIR/corsairs-gltf-tests/sample.gltf` в Blender (через Blender MCP
или вручную: File → Import → glTF 2.0). Модель должна импортироваться без
ошибок, иметь ненулевые размеры и нормали, направленные наружу.

Это ручная проверка: автоматический тест подтверждает синтаксис, но не то,
что геометрия выглядит правильно.

- [ ] **Шаг 9: Коммит**

```bash
cd /Users/ivan/code/TalesOfPirate
git add tools/AssetConverter
git commit -m "AssetConverter: запись glTF 2.0 с преобразованием системы координат"
```

---

## Task 6: CLI и пакетный прогон с отчётом

**Файлы:**
- Создать: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ConversionReport.h`
- Изменить: `tools/AssetConverter/src/ConversionReport.cpp` (заменить заглушку)
- Изменить: `tools/AssetConverter/src/Main.cpp` (заменить заглушку)
- Создать: `tools/AssetConverter/tests/TestConversionReport.cpp`
- Изменить: `tools/AssetConverter/CMakeLists.txt` (добавить новый тестовый файл)

**Интерфейсы:**
- Потребляет: `LgoStatus`, `ToString(LgoStatus)`, `GltfStatus`, `ParseLgo`, `WriteGltf`.
- Предоставляет: `struct ReportEntry { std::string Path; std::string Status; std::string Detail; }`;
  класс `ConversionReport` с методами `AddSuccess(std::string_view path, std::string_view status)`,
  `AddFailure(std::string_view path, std::string_view status, std::string_view detail)`,
  `SuccessCount()`, `FailureCount()`, `TotalCount()`, `Summary()`, `WriteCsv(const std::filesystem::path&)`.

- [ ] **Шаг 1: Написать падающий тест**

Создать `tools/AssetConverter/tests/TestConversionReport.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ConversionReport.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

CORSAIRS_TEST(ConversionReport_CountsSuccessesAndFailures) {
    AC::ConversionReport report;
    report.AddSuccess("a.lgo", "OK");
    report.AddSuccess("b.lgo", "OK_WITH_TRAILING_DATA");
    report.AddFailure("c.lgo", "VERSION_UNKNOWN", "version=0xDEADBEEF");

    REQUIRE_EQ(report.SuccessCount(), 2u);
    REQUIRE_EQ(report.FailureCount(), 1u);
    REQUIRE_EQ(report.TotalCount(), 3u);
}

CORSAIRS_TEST(ConversionReport_SummaryGroupsByStatus) {
    AC::ConversionReport report;
    report.AddSuccess("a.lgo", "OK");
    report.AddSuccess("b.lgo", "OK");
    report.AddFailure("c.lgo", "VERSION_UNKNOWN", "");

    const std::string summary = report.Summary();
    REQUIRE(summary.find("OK") != std::string::npos);
    REQUIRE(summary.find("2") != std::string::npos);
    REQUIRE(summary.find("VERSION_UNKNOWN") != std::string::npos);
}

CORSAIRS_TEST(ConversionReport_CsvContainsEveryEntry) {
    AC::ConversionReport report;
    report.AddSuccess("a.lgo", "OK");
    report.AddFailure("c.lgo", "VERSION_UNKNOWN", "деталь, с запятой");

    const std::filesystem::path csv =
        std::filesystem::temp_directory_path() / "corsairs-report-test.csv";
    std::filesystem::remove(csv);

    REQUIRE(report.WriteCsv(csv));
    REQUIRE(std::filesystem::exists(csv));

    const auto bytes = AC::ReadWholeFile(csv);
    REQUIRE(bytes.has_value());
    const std::string text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};

    REQUIRE(text.find("a.lgo") != std::string::npos);
    REQUIRE(text.find("c.lgo") != std::string::npos);
    // Поле с запятой обязано быть в кавычках, иначе CSV разъедется.
    REQUIRE(text.find("\"деталь, с запятой\"") != std::string::npos);
}

} // namespace
```

В `tools/AssetConverter/CMakeLists.txt` заменить

```cmake
    tests/TestGltfWriter.cpp
)
```

на

```cmake
    tests/TestGltfWriter.cpp
    tests/TestConversionReport.cpp
)
```

- [ ] **Шаг 2: Запустить и убедиться, что не собирается**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Ожидается: FAIL — `ConversionReport.h` не найден.

- [ ] **Шаг 3: Написать ConversionReport**

Создать `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/ConversionReport.h`:

```cpp
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

struct ReportEntry {
    std::string Path;
    std::string Status;
    std::string Detail;
    bool Succeeded{false};
};

// Накапливает результат пакетного прогона. Ни один файл не пропускается молча:
// каждый обработанный файл попадает в отчёт со своим статусом.
class ConversionReport {
public:
    void AddSuccess(std::string_view path, std::string_view status);
    void AddFailure(std::string_view path, std::string_view status, std::string_view detail);

    [[nodiscard]] std::size_t SuccessCount() const {
        return _successCount;
    }

    [[nodiscard]] std::size_t FailureCount() const {
        return _entries.size() - _successCount;
    }

    [[nodiscard]] std::size_t TotalCount() const {
        return _entries.size();
    }

    // Человекочитаемая сводка: количество файлов по каждому статусу.
    [[nodiscard]] std::string Summary() const;

    [[nodiscard]] bool WriteCsv(const std::filesystem::path& path) const;

private:
    std::vector<ReportEntry> _entries;
    std::size_t _successCount{0};
};

} // namespace Corsairs::Tools::AssetConverter
```

Заменить содержимое `tools/AssetConverter/src/ConversionReport.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/ConversionReport.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>

namespace Corsairs::Tools::AssetConverter {

namespace {

// Экранирует поле CSV: кавычки удваиваются, поле берётся в кавычки, если
// содержит запятую, кавычку или перевод строки.
std::string EscapeCsv(std::string_view field) {
    const bool needsQuotes =
        field.find(',') != std::string_view::npos ||
        field.find('"') != std::string_view::npos ||
        field.find('\n') != std::string_view::npos;

    if (!needsQuotes) {
        return std::string{field};
    }

    std::string out;
    out += '"';
    for (char c : field) {
        if (c == '"') {
            out += "\"\"";
        }
        else {
            out += c;
        }
    }
    out += '"';
    return out;
}

} // namespace

void ConversionReport::AddSuccess(std::string_view path, std::string_view status) {
    _entries.push_back(ReportEntry{std::string{path}, std::string{status}, {}, true});
    ++_successCount;
}

void ConversionReport::AddFailure(std::string_view path, std::string_view status,
                                  std::string_view detail) {
    _entries.push_back(
        ReportEntry{std::string{path}, std::string{status}, std::string{detail}, false});
}

std::string ConversionReport::Summary() const {
    std::map<std::string, std::size_t> byStatus;
    for (const ReportEntry& entry : _entries) {
        ++byStatus[entry.Status];
    }

    std::string out = std::format("Всего файлов: {}, успешно: {}, с ошибкой: {}\n",
                                  TotalCount(), SuccessCount(), FailureCount());
    for (const auto& [status, count] : byStatus) {
        out += std::format("  {:<28} {}\n", status, count);
    }
    return out;
}

bool ConversionReport::WriteCsv(const std::filesystem::path& path) const {
    std::ofstream stream{path, std::ios::trunc};
    if (!stream) {
        return false;
    }

    stream << "path,status,detail\n";
    for (const ReportEntry& entry : _entries) {
        stream << EscapeCsv(entry.Path) << ','
               << EscapeCsv(entry.Status) << ','
               << EscapeCsv(entry.Detail) << '\n';
    }
    return static_cast<bool>(stream);
}

} // namespace Corsairs::Tools::AssetConverter
```

- [ ] **Шаг 4: Написать CLI**

Заменить содержимое `tools/AssetConverter/src/Main.cpp`:

```cpp
#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ConversionReport.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

void PrintUsage() {
    std::cout <<
        "Использование:\n"
        "  AssetConverter <входной-каталог> <выходной-каталог> [--report <файл.csv>]\n"
        "\n"
        "Рекурсивно обходит входной каталог, конвертирует каждый .lgo в glTF 2.0\n"
        "и сохраняет результат с той же относительной структурой каталогов.\n"
        "Код возврата: 0 — все файлы обработаны, 1 — есть ошибки, 2 — неверные аргументы.\n";
}

// Конвертирует один файл, добавляя результат в отчёт. Возвращает false при
// любой ошибке разбора или записи.
bool ConvertOne(const std::filesystem::path& input, const std::filesystem::path& output,
                std::string_view relative, AC::ConversionReport& report) {
    const auto bytes = AC::ReadWholeFile(input);
    if (!bytes) {
        report.AddFailure(relative, "FILE_READ_FAILED", "файл не открылся");
        return false;
    }

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    if (!obj) {
        report.AddFailure(relative, AC::ToString(diag.Status), diag.Detail);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        report.AddFailure(relative, "OUTPUT_DIR_FAILED", ec.message());
        return false;
    }

    std::string detail;
    const AC::GltfStatus status = AC::WriteGltf(*obj, output, detail);
    if (status != AC::GltfStatus::OK) {
        const std::string_view name =
            status == AC::GltfStatus::EMPTY_MESH ? "EMPTY_MESH" : "WRITE_FAILED";
        report.AddFailure(relative, name, detail);
        return false;
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage();
        return 2;
    }

    const std::filesystem::path inputRoot{argv[1]};
    const std::filesystem::path outputRoot{argv[2]};
    std::filesystem::path reportPath;

    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--report" && i + 1 < argc) {
            reportPath = argv[i + 1];
            ++i;
        }
        else {
            PrintUsage();
            return 2;
        }
    }

    if (!std::filesystem::is_directory(inputRoot)) {
        std::cout << std::format("Входной каталог не найден: {}\n", inputRoot.string());
        return 2;
    }

    AC::ConversionReport report;

    for (const auto& entry : std::filesystem::recursive_directory_iterator{inputRoot}) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string extension = entry.path().extension().string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (extension != ".lgo") {
            continue;
        }

        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), inputRoot);
        std::filesystem::path output = outputRoot / relative;
        output.replace_extension(".gltf");

        ConvertOne(entry.path(), output, relative.generic_string(), report);
    }

    std::cout << report.Summary();

    if (!reportPath.empty()) {
        if (!report.WriteCsv(reportPath)) {
            std::cout << std::format("Не удалось записать отчёт: {}\n", reportPath.string());
            return 1;
        }
        std::cout << std::format("Отчёт: {}\n", reportPath.string());
    }

    return report.FailureCount() == 0 ? 0 : 1;
}
```

- [ ] **Шаг 5: Собрать и запустить тесты**

```bash
cd /Users/ivan/code/TalesOfPirate/tools/AssetConverter
cmake --build build && ./build/AssetConverterTests
```

Ожидается: 27 тестов, провалено 0.

- [ ] **Шаг 6: Прогнать все 6702 файла**

```bash
cd /Users/ivan/code/TalesOfPirate
./tools/AssetConverter/build/AssetConverter \
    Client/model \
    /tmp/corsairs-gltf-out \
    --report /tmp/corsairs-lgo-report.csv
```

Ожидается: сводка с разбивкой по статусам и CSV-отчёт. Ненулевой код возврата
здесь **не является провалом задачи** — часть исходных файлов заведомо битая, и
цель прогона в том, чтобы узнать, сколько именно и каких.

Разобрать отчёт:

```bash
tail -n +2 /tmp/corsairs-lgo-report.csv | cut -d, -f2 | sort | uniq -c | sort -rn
```

- [ ] **Шаг 7: Записать результаты прогона в спеку**

Дописать в `docs/superpowers/specs/2026-08-05-unreal-port-design.md`, в раздел
«Открытые вопросы», фактические числа: сколько файлов дали `OK`, сколько
`OK_WITH_TRAILING_DATA`, сколько `VERSION_UNSUPPORTED` и сколько реальных
ошибок. От этих чисел зависит, нужна ли поддержка версий ниже `0x1004`
отдельной задачей.

- [ ] **Шаг 8: Коммит**

```bash
cd /Users/ivan/code/TalesOfPirate
git add tools/AssetConverter docs/superpowers/specs
git commit -m "AssetConverter: CLI, пакетный прогон и отчёт по конвертации"
```

---

## Границы плана

Этот план закрывает этапы 0 и 1 спеки: конвертер собирается, разбирает `.lgo`
версий `0x1004`–`0x1005` и пишет статическую геометрию в glTF 2.0.

**Осознанно не входит** и планируется отдельно после того, как отчёт прогона даст
фактические числа:

- Блоки `helper` и `anim` разбираются только по размеру (пропускаются). Helper-
  данные нужны для точек крепления оружия — это этап 2.
- Скиннинг (`Blends`, `BoneIndices`) читается, но в glTF не пишется: без скелета
  из `.lab` он бессмысленен — этап 2.
- Материалы разбираются, но в glTF не пишутся: по решению из спеки материалы
  делаются средствами UE, а не переносятся. В glTF попадает только геометрия;
  имена текстур доступны через `LgoMaterial::TextureName` для будущего манифеста.
- Версии `.lgo` ниже `0x1004` не поддерживаются.
- Форматы `.lmo`, `.lab`, `.obj`, `.map`, `.eff`, `.par` — последующие этапы.
