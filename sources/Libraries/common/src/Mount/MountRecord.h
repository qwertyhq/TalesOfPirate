#pragma once

#include "Database/TableData.h"
// Windows-заголовки только на Windows; на POSIX — словарь совместимости.
#include "PlatformCompat.h"
#include <array>

// Запись таблицы маунтов (mountinfo)

namespace Corsairs::Common::Mount {

class CMountInfo : public EntityData
{
public:
	CMountInfo() = default;

	short mountID{};
	short boneID{};
	std::array<short, 4> height{ /* Lance, Carsise, Phyllis, Ami */};
	short offsetX{};
	short offsetY{};
	std::array<short, 4> poseID{ /* Lance, Carsise, Phyllis, Ami */};
};

} // namespace Corsairs::Common::Mount

