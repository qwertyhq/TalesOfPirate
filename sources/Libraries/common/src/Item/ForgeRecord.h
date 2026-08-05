// ForgeRecord.h Created by knight-gongjian 2005.1.24.
//---------------------------------------------------------
#pragma once

#ifndef _FORGERECORD_H_
#define _FORGERECORD_H_

// tchar.h нужен только на Windows; TCHAR даёт словарь совместимости.
#include "PlatformCompat.h"
#include "util.h"
#include "Database/TableData.h"

//---------------------------------------------------------
#define FORGE_MAXNUM_ITEM				6 //


namespace Corsairs::Common::Item {

class CForgeRecord : public EntityData
{
public:
	BYTE byLevel;	//
	BYTE byFailure; //
	BYTE byRate;	//
	BYTE byParam;	//
	DWORD dwMoney;  //

	//
	struct FORGE_ITEM
	{
		USHORT sItem;	// ID
		BYTE   byNum;	//
		BYTE   byParam; //
	};
	FORGE_ITEM ForgeItem[FORGE_MAXNUM_ITEM];
};

//---------------------------------------------------------
#endif // _FORGERECORD_H_

} // namespace Corsairs::Common::Item

