//=============================================================================
// FileName: EntitySpawn.cpp
// Creater: ZhangXuedong
// Date: 2004.09.10
// Comment: CChaSpawn class
//=============================================================================
#include "Core/stdafx.h"
namespace Corsairs::Common::NPC {}
using namespace Corsairs::Common::NPC;
#include "Entity/EntitySpawn.h"
#include "Character/Character.h"
#include "Network/NetCommand.h"
#include "Network/NetRetCode.h"
#include "Core/RoleCommon.h"
#include "Network/CompCommand.h"
#include "Core/CommFunc.h"
#include "App/GameAppNet.h"
#include "World/SubMap.h"
#include "Network/CompCommand.h"
#include "App/GameApp.h"
#include "NPC/NPC.h"

#include <string>

extern const char* GetResPath(const char* pszRes);
//=============================================================================
CChaSpawn::CChaSpawn() = default;

CChaSpawn::~CChaSpawn() = default;

bool CChaSpawn::Init(const char *szMapName, const char *szSpawnTable)
{
	m_pCMap = nullptr;

	if (szMapName == nullptr || szMapName[0] == '\0' || szSpawnTable == nullptr) {
		ThrowRuntimeError(RES_STRING(GM_ENTITYSPAWN_CPP_00001));
	}

	m_strMapName = szMapName;
	m_lCount = 0;

	m_pStore = std::make_unique<MonRefRecordStore>();

	std::string txtPath = std::string(GetResPath(szSpawnTable)) + ".txt";
	return m_pStore->Load(txtPath.c_str());
}

long CChaSpawn::Load(SubMap *pCMap)
{
	m_pCMap = pCMap;
	m_lCount = 0;

	if (!m_pStore) {
		ToLogService("errors", LogLevel::Error,
					 "CChaSpawn::Load: store is null (map='{}')", m_strMapName);
		return 0;
	}

	const Corsairs::Util::Rect& area = pCMap->GetRange();
	bool reachedCap = false;

	// Размер блочной клетки obstacle-карты в мировых единицах — для перевода
	// мировых координат в координаты IsBlock. Кешируем один раз, размер карты
	// в течение Load не меняется.
	const auto cellW = pCMap->GetBlockCellWidth();
	const auto cellH = pCMap->GetBlockCellHeight();
	const bool blockGridValid = (cellW > 0 && cellH > 0);

	// Сколько раз перекатывать рандом, если первая точка попадает в obstacle.
	// 16 даёт ~99.998% покрытия даже при 50% obstacle-density региона
	// (1 - 0.5^16 ≈ 99.998%). Больше — пустая трата CPU при старте карты.
	constexpr int kMaxWalkableRetries = 16;

	auto randInRange = [](long lo, long hi) -> long {
		if (lo >= hi) {
			return lo;
		}
		long lRand = rand();
		long lSub  = hi - lo;
		long lBase = (lSub / RAND_MAX > 0) ? (lRand % (lSub / RAND_MAX + 1) * RAND_MAX) : 0;
		return lBase + lRand % (lSub - lBase) + lo;
	};

	m_pStore->ForEach([&](CMonRefRecord& rec) {
		if (reachedCap) return;

		CMonRefRecord* pMonRefRecord = &rec;
		long lNum = 0;
		long lObstacleFails = 0; // сколько точек пропустили из-за obstacle во всех retry
		long lOtherFails    = 0; // walkable, но ChaSpawn вернул null (редко, диагностика)

		for (int j = 0; j < defMAX_REGION_MONSTER_TYPE; j++) {
			for (int k = 0; k < pMonRefRecord->lMonster[j][1]; k++) {
				short sAngle = pMonRefRecord->sAngle;
				if (sAngle == -1) {
					sAngle = rand() % 360;
				}

				Corsairs::Util::Point l_pos{};
				bool walkable = false;
				for (int retry = 0; retry < kMaxWalkableRetries; ++retry) {
					l_pos.X = randInRange(pMonRefRecord->SRegion[0].X, pMonRefRecord->SRegion[1].X);
					l_pos.Y = randInRange(pMonRefRecord->SRegion[0].Y, pMonRefRecord->SRegion[1].Y);
					if (!blockGridValid) {
						// Нет валидного block-grid — доверяем точке, ChaSpawn проверит сам.
						walkable = true;
						break;
					}
					if (!pCMap->IsBlock(l_pos.X / cellW, l_pos.Y / cellH)) {
						walkable = true;
						break;
					}
				}

				if (!walkable) {
					++lObstacleFails;
					continue;
				}

				CCharacter* pCCha = pCMap->ChaSpawn(
					pMonRefRecord->lMonster[j][0], EChaCtrlType::NONE, sAngle, &l_pos);
				if (pCCha) {
					pCCha->SetResumeTime(pMonRefRecord->lMonster[j][3] * 1000);
					m_lCount++;
					lNum++;
					if (m_lCount >= g_Config.m_nMaxCha) {
						ToLogService("common", LogLevel::Warning,
									 "Character count reached maximum, stopping spawn");
						reachedCap = true;
						return;
					}
				}
				else {
					// Точка прошла walkable pre-check, но ChaSpawn всё равно вернул null
					// (entity pool overflow, отсутствие CChaRecord, neighbour overlap, и т.п.).
					// Это нормально редкая ошибка — оставляем индивидуальный лог для диагностики.
					++lOtherFails;
					ToLogService("errors", LogLevel::Error,
						"character born error (walkable but spawn failed): map {}[{}, {}], "
						"character hatch list number {}, character list number {}, born position[{}, {}]",
						pCMap->GetName(), area.Width(), area.Height(),
						pMonRefRecord->Id, pMonRefRecord->lMonster[j][0], l_pos.X, l_pos.Y);
				}
			}
		}
		if (lObstacleFails > 0) {
			ToLogService("common", LogLevel::Warning,
				"spawn region in map '{}' (record id={}): {} positions skipped (obstacle), "
				"{} other failures, {} succeeded",
				pCMap->GetName(), pMonRefRecord->Id, lObstacleFails, lOtherFails, lNum);
		}
		ToLogService("common", "entry {} character number:\t{}", pMonRefRecord->Id, lNum);
	});

	return 1;
}

long CChaSpawn::Reload()
{
	// 
	return 0;
}

//=============================================================================
CMapSwitchEntitySpawn::CMapSwitchEntitySpawn() = default;

CMapSwitchEntitySpawn::~CMapSwitchEntitySpawn() = default;

bool CMapSwitchEntitySpawn::Init(const char *szMapName, const char *szSpawnTable)
{
	m_pCMap = nullptr;

	if (szMapName == nullptr || szMapName[0] == '\0' || szSpawnTable == nullptr) {
		ThrowRuntimeError(RES_STRING(GM_ENTITYSPAWN_CPP_00005));
	}

	m_strMapName = szMapName;
	m_pStore = std::make_unique<Corsairs::Common::World::SwitchMapRecordStore>();

	std::string txtPath = std::string(GetResPath(szSpawnTable)) + ".txt";
	return m_pStore->Load(txtPath.c_str());
}

long CMapSwitchEntitySpawn::Load(SubMap *pCMap)
{
	m_pCMap = pCMap;

	if (!m_pStore) {
		ToLogService("errors", LogLevel::Error,
					 "CMapSwitchEntitySpawn::Load: store is null (map='{}')", m_strMapName);
		return 0;
	}

	SItemGrid SItemCont;
	m_pStore->ForEach([&](Corsairs::Common::World::CSwitchMapRecord& rec) {
		Corsairs::Common::World::CSwitchMapRecord* pCSwitchMapRecord = &rec;

		SItemCont.sID = (short)pCSwitchMapRecord->lEntityID;
		SItemCont.sNum = 1;
		SItemCont.SetDBParam(-1, 0);
		SItemCont.chForgeLv = 0;
		SItemCont.SetInstAttrInvalid();

		CEvent CEvtCont;
		CEvtCont.SetID((short)pCSwitchMapRecord->lEventID);
		CEvtCont.SetTouchType(Corsairs::Common::World::enumEVENTT_RANGE);
		CEvtCont.SetExecType(Corsairs::Common::World::enumEVENTE_SMAP_ENTRY);
		CEvtCont.SetTableRec(pCSwitchMapRecord);  // адрес стабилен: store не перевыделяет vector после Load

		CItem* pCItem = pCMap->ItemSpawn(
			&SItemCont,
			pCSwitchMapRecord->SEntityPos.X, pCSwitchMapRecord->SEntityPos.Y,
			enumITEM_APPE_NATURAL, 0,
			g_pCSystemCha->GetID(), g_pCSystemCha->GetHandle(),
			-1, -1, &CEvtCont);
		if (pCItem) {
			pCItem->SetOnTick(0);
		}
	});

	return 1;
}

long CMapSwitchEntitySpawn::Reload()
{
	return 0;
}

//=============================================================================
CNpcSpawn::CNpcSpawn() = default;

CNpcSpawn::~CNpcSpawn()
{
	Clear();
}

bool CNpcSpawn::Init(const char *szMapName, const char *szSpawnTable)
{
	if (szMapName == nullptr || szMapName[0] == '\0' || szSpawnTable == nullptr) {
		char szTemp[128];
		std::snprintf(szTemp, sizeof(szTemp), RES_STRING(GM_ENTITYSPAWN_CPP_00007), szSpawnTable ? szSpawnTable : "", 0);
		ThrowRuntimeError(szTemp);
	}

	m_strMapName = szMapName;

	m_pNpcStore = std::make_unique<NpcRecordStore>();

	std::string txtPath = std::string(GetResPath(szSpawnTable)) + ".txt";
	return m_pNpcStore->Load(txtPath.c_str());
}

void CNpcSpawn::Clear()
{
	m_pNpcStore.reset();
}

Corsairs::Common::Mission::CNpc* CNpcSpawn::FindNpc( const char szName[] )
{
	for( int i = 0; i < m_sNumNpc; i++ )
	{
		if( strcmp( m_NpcList[i]->GetNpcName(), szName ) == 0 )
			return m_NpcList[i];
	}
	return NULL;
}

long CNpcSpawn::Load( SubMap& submap )
{
	// NPC
	memset( m_NpcList, 0, sizeof(Corsairs::Common::Mission::CNpc*)*ROLE_MAXNUM_MAPNPC );
	m_sNumNpc = 0;

	ToLogService("common", "Loading NPCs for map '{}'... ", m_strMapName);

	if (!m_pNpcStore) {
		ToLogService("errors", LogLevel::Error, "CNpcSpawn::Load: store is null (map='{}')", m_strMapName);
		return 0;
	}

	bool hasError = false;
	m_pNpcStore->ForEach([&](CNpcRecord& rec) {
		CNpcRecord* pNpcRecord = &rec;

		CChaRecord* pCharRecord = GetChaRecordInfo(pNpcRecord->sCharID);
		if (pCharRecord == nullptr) {
			hasError = true;
			C_PRINT("\nerror: NPC %d model %d unfound!", (int)pNpcRecord->Id, pNpcRecord->sCharID);
			ToLogService("errors", LogLevel::Error,
						 "initialization map errornot find appoint ID roll attribute informationID = {}",
						 pNpcRecord->sCharID);
			return;
		}

		switch (pNpcRecord->sNpcType) {
			case Corsairs::Common::Mission::CNpc::TALK: {
				Corsairs::Common::Mission::CTalkNpc* pTalk = g_pGameApp->GetNewTNpc();
				if (pTalk == nullptr) {
					break;
				}
				if (pTalk->Load(*pNpcRecord, *pCharRecord) == FALSE) {
					pTalk->Free();
					return;
				}
				Corsairs::Util::Square SShape = {{static_cast<std::int32_t>(pNpcRecord->dwxPos0), static_cast<std::int32_t>(pNpcRecord->dwyPos0)}, static_cast<std::int32_t>(pCharRecord->Radii)};
				if (!submap.Enter(&SShape, pTalk)) {
					pTalk->Free();
					return;
				}
				if (m_sNumNpc < ROLE_MAXNUM_MAPNPC) {
					m_NpcList[m_sNumNpc++] = pTalk;
				}
			} break;
			default:
				break;
		}
	});

	if (!hasError) {
		C_PRINT("success!\n");
	}
	else {
		ToLogService("common", "");
	}
	return 0;
}

long CNpcSpawn::Reload()
{
	return 0;
}

CNpcRecord* CNpcSpawn::GetNpcInfo( USHORT sNpcID )
{
	if (m_pNpcStore) {
		return m_pNpcStore->Get(static_cast<int>(sNpcID));
	}
	return nullptr;
}

BOOL CNpcSpawn::SummonNpc( const char szNpc[], USHORT sAreaID, USHORT sTime )
{
	for( USHORT i = 0; i < m_sNumNpc; i++ )
	{
		if( m_NpcList[i] && m_NpcList[i]->GetIslandID() == sAreaID && strcmp( m_NpcList[i]->GetName(), szNpc ) == 0 )
		{
			m_NpcList[i]->Summoned( sTime );
			//return TRUE;
		}
	}
	return FALSE;
}
