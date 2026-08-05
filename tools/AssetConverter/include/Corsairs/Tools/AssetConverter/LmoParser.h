#pragma once

#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

// Тип записи в оглавлении .lmo. Источник — MODEL_OBJ_TYPE_* в
// sources/Engine/Model/lwExpObj.h.
enum class ModelObjType : std::uint32_t {
    GEOMETRY = 1,
    HELPER   = 2,
};

#pragma pack(push, 1)

// Запись оглавления: тип содержимого, абсолютное смещение в файле и длина.
struct ModelObjEntry {
    std::uint32_t Type;
    std::uint32_t Addr;
    std::uint32_t Size;
};

#pragma pack(pop)

static_assert(sizeof(ModelObjEntry) == 12, "ModelObjEntry: раскладка на диске 12 байт");

// Модель — набор геометрических объектов плюс общий helper-блок. В отличие от
// .lgo, где объект один, здесь их может быть несколько, и каждый несёт
// собственную локальную матрицу в GeomObjHeader::MatLocal.
struct LmoModel {
    std::uint32_t Version{0};
    std::vector<LgoGeomObj> Objects;
    LgoHelper Helper;
};

// Разбирает .lmo. Диагностика переиспользует статусы .lgo: тела объектов
// внутри .lmo имеют ровно тот же формат.
[[nodiscard]] std::optional<LmoModel> ParseLmo(std::span<const std::uint8_t> bytes,
                                               LgoDiagnostics& diag);

// Заполняет `MatModel` каждого объекта его положением в пространстве модели:
// `MatModel = MatLocal * MatModel(родителя)`, как делает `lwNodeObject` в
// движке. Объекты без родителя сохраняют собственную `MatLocal`.
//
// Родитель адресуется полем `Id`, а не индексом, и в исходных данных `Id`
// иногда повторяются — при совпадении берётся первый. Цикл и ссылка на
// отсутствующего родителя обрываются: объект остаётся корневым. Это хуже
// потерянного сдвига, но не зацикливает конвертацию.
//
// Вызывается из ParseLmo; отдельно объявлена ради проверки в тестах.
void ResolveModelMatrices(std::vector<LgoGeomObj>& objects);

} // namespace Corsairs::Tools::AssetConverter
