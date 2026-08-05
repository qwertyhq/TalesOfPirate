//======================================================================================================================
// FileName: CharacterRecord.h
// Creater: ZhangXuedong
// Date: 2004.09.01
// Comment: CChaRecordSet class
//======================================================================================================================

#pragma once

#include <array>
#include <cstdint>
#include <source_location>
#include <string>
// tchar.h нужен только на Windows; TCHAR даёт словарь совместимости.
#include "PlatformCompat.h"
#include "util.h"
#include "Database/TableData.h"

namespace Corsairs::Common::Character {

enum class EChaModalType : std::int8_t {
	MAIN_CHA = 1,
	BOAT,
	EMPL,
	OTHER,
};

enum class EChaCtrlType : std::int8_t {
	NONE			= 0,

	PLAYER			= 1,

	NPC				= 2,
	NPC_EVENT		= 3,

	MONS			= 5,
	MONS_TREE		= 6,
	MONS_MINE		= 7,
	MONS_FISH		= 8,
	MONS_DBOAT		= 9,

	PLAYER_PET		= 10,

	MONS_REPAIRABLE = 17,
};

enum class EChaAiType : std::int32_t {
	NONE			= 0,
	ATTACK_PASSIVE	= 1,
	ATTACK_ACTIVE	= 2,
};

// Sentinel-байт для пустых ячеек массивов Item/TaskItem/SkinInfo/ItemType (0xFF / -1).
inline constexpr std::int8_t kChaRecordKeyValue = static_cast<std::int8_t>(0xFF);

inline constexpr std::size_t kChaSkinNum			= 8;
inline constexpr std::size_t kChaInitSkillNum		= 11;	// 0,1-10
inline constexpr std::size_t kChaInitItemNum		= 15;	// Edit by Mdrst
inline constexpr std::size_t kChaItemKindNum		= 20;
inline constexpr std::size_t kChaDieEffectNum		= 3;
inline constexpr std::size_t kChaBirthEffectNum		= 3;
inline constexpr std::size_t kChaHpEffectNum		= 3;
inline constexpr std::size_t kChaFeffNum			= 4;
inline constexpr std::size_t kChaEffectActionNum	= 3;
inline constexpr std::size_t kChaScalingNum			= 3;

// TypeID и Name записи живут в базе EntityData (поля Id / DataName), здесь не дублируем.
class ChaRecord : public EntityData
{
public:
	std::string		IconName;
	EChaModalType	ModalType;
	EChaCtrlType	CtrlType;
	std::int16_t	Model;
	std::int16_t	SuitID;
	std::int16_t	SuitNum;
	std::array<std::int16_t, kChaSkinNum>				SkinInfo;
	std::array<std::int16_t, kChaFeffNum>				FeffID;
	std::int16_t	EeffID;
	std::array<std::int16_t, kChaEffectActionNum>		EffectActionID;
	std::int16_t	Shadow;
	std::int16_t	ActionId;
	std::int8_t		Diaphaneity;
	std::int16_t	Footfall;
	std::int16_t	Whoop;
	std::int16_t	Dirge;
	std::int8_t		ControlAble;
	std::int8_t		Territory;
	std::int16_t	SeaHeight;
	std::array<std::int16_t, kChaItemKindNum>			ItemType;
	float			Length;			// (был fLengh — опечатка исправлена)
	float			Width;
	float			Height;
	std::int16_t	Radii;
	std::array<std::int8_t, kChaBirthEffectNum>			BirthBehave;
	std::array<std::int8_t, kChaDieEffectNum>			DiedBehave;
	std::int16_t	BornEff;
	std::int16_t	DieEff;
	std::int16_t	Dormancy;
	std::int8_t		DieAction;
	std::array<std::int32_t, kChaHpEffectNum>			HpEffect;
	bool			_IsFace;		// (приватный геттер: IsFace())
	bool			_IsCyclone;		// (приватный геттер: IsCyclone())
	std::int32_t	Script;
	std::int32_t	Weapon;
	std::array<std::array<std::int32_t, 2>, kChaInitSkillNum>	Skill;
	std::array<std::array<std::int32_t, 2>, kChaInitItemNum>	Item;
	std::array<std::array<std::int32_t, 2>, kChaInitItemNum>	TaskItem;
	std::int32_t	MaxShowItem;
	float			AllShow;
	std::int32_t	Prefix;
	EChaAiType		AiNo;
	std::int8_t		CanTurn;
	std::int32_t	Vision;
	std::int32_t	Noise;
	std::int32_t	GetExp;
	bool			Light;

	std::int32_t	MobExp;
	std::int32_t	Lv;
	std::int32_t	MxHp;
	std::int32_t	Hp;
	std::int32_t	MxSp;
	std::int32_t	Sp;
	std::int32_t	MnAtk;
	std::int32_t	MxAtk;
	std::int32_t	PDef;
	std::int32_t	Def;
	std::int32_t	Hit;
	std::int32_t	Flee;
	std::int32_t	Crt;
	std::int32_t	Mf;
	std::int32_t	HRec;
	std::int32_t	SRec;
	std::int32_t	ASpd;
	std::int32_t	ADis;
	std::int32_t	CDis;
	std::int32_t	MSpd;
	std::int32_t	Col;
	std::int32_t	Str;	// strength
	std::int32_t	Agi;	// agility
	std::int32_t	Dex;	// dexterity
	std::int32_t	Con;	// constitution	hp
	std::int32_t	Sta;	// stamina		sp
	std::int32_t	Luk;	// luck
	std::int32_t	LHandVal;
	std::string		Guild;
	std::string		Title;
	std::string		Job;
	std::int32_t	CExp;
	std::int32_t	NExp;
	std::int32_t	Fame;
	std::int32_t	Ap;
	std::int32_t	Tp;		// technique point
	std::int32_t	Gd;
	std::int32_t	Spri;
	std::int32_t	Stor;
	std::int32_t	MxSail;
	std::int32_t	Sail;
	std::int32_t	Stasa;
	std::int32_t	Scsm;

	std::int32_t	TStr;
	std::int32_t	TAgi;
	std::int32_t	TDex;
	std::int32_t	TCon;
	std::int32_t	TSta;
	std::int32_t	TLuk;
	std::int32_t	TMxHp;
	std::int32_t	TMxSp;
	std::int32_t	TAtk;
	std::int32_t	TDef;
	std::int32_t	THit;
	std::int32_t	TFlee;
	std::int32_t	TMf;
	std::int32_t	TCrt;
	std::int32_t	THRec;
	std::int32_t	TSRec;
	std::int32_t	TASpd;
	std::int32_t	TADis;
	std::int32_t	TSpd;
	std::int32_t	TSpri;
	std::int32_t	TScsm;

	// added by clp
	std::array<float, kChaScalingNum>	Scaling;

public:
	bool	HaveEffectFog() const				{ return _haveEffectFog;	}
	int		GetEffectFog( int i ) const			{ return HpEffect.at(i);	}

	bool	IsFace() const						{ return _IsFace;			}
	bool	IsCyclone() const					{ return _IsCyclone;		}

	void	RefreshPrivateData();

private:
	bool	_haveEffectFog;

};

ChaRecord* GetChaRecordInfo(int nTypeID, const std::source_location& loc = std::source_location::current());


} // namespace Corsairs::Common::Character
