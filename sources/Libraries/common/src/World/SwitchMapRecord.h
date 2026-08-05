//=============================================================================
// FileName: SwitchMapRecord.h
// Creater: ZhangXuedong
// Date: 2004.11.23
// Comment: CSwitchMapRecord — DTO точки телепорта между картами (использует SwitchMapRecordStore).
//=============================================================================

#ifndef SWITCHMAPRECORD_H
#define SWITCHMAPRECORD_H

// tchar.h нужен только на Windows; TCHAR даёт словарь совместимости.
#include "PlatformCompat.h"
#include "Database/TableData.h"
#include "point.h"

#define defMAP_NAME_LEN		16

// Одна запись = один невидимый предмет-триггер на карте, при касании переводит на другую карту.
// Адрес записи сохраняется в CEvent::pTableRec — поэтому после Load стор не должен перевыделять вектор.

namespace Corsairs::Common::World {

class CSwitchMapRecord : public EntityData
{
public:
	long	lID;                           // дубликат nID
	long	lEntityID;                     // ID предмета-триггера (ItemInfo)
	long	lEventID;                      // ID события
	Corsairs::Util::Point	SEntityPos;                    // позиция триггера на текущей карте
	short	sAngle;                        // направление (−1 = случайное)
	_TCHAR	szTarMapName[defMAP_NAME_LEN]; // имя целевой карты
	Corsairs::Util::Point	STarPos;                       // позиция появления на целевой карте
};


} // namespace Corsairs::Common::World

#endif
