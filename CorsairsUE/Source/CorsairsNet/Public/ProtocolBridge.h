#pragma once

#include "CoreMinimal.h"

namespace Corsairs::Protocol
{

// Проверяет, что сериализация протокола даёт big-endian и переживает
// round-trip. Совместимость с F#-серверами держится на этом порядке байт.
CORSAIRSNET_API bool CheckByteOrder();

} // namespace Corsairs::Protocol
