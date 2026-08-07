#include "CorsairsLookAdapter.h"

FCorsairsCharacterLook MakeCharacterLook(
	const Corsairs::Net::Msg::ChaLookInfo& Source)
{
	FCorsairsCharacterLook Result;
	Result.SynType = static_cast<int32>(Source.synType);
	Result.TypeId = static_cast<int32>(Source.typeId);
	Result.HairId = static_cast<int32>(Source.hairId);
	Result.bIsBoat = Source.isBoat;
	Result.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);
	for (int32 Index = 0; Index < CorsairsEquipSlotCount; ++Index)
	{
		Result.EquipIds[Index] = static_cast<int32>(Source.equips[Index].id);
	}
	return Result;
}
