#pragma once

// tchar.h нужен только на Windows; TCHAR даёт словарь совместимости.
#include "PlatformCompat.h"
#include "util.h"
#include "Database/TableData.h"


namespace Corsairs::Common::Progression {

class CLifeLvRecord : public EntityData
{
public:
	long	lID;			//
	short	sLevel;			//
	unsigned long	ulExp;	//
};

} // namespace Corsairs::Common::Progression

