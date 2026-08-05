//=============================================================================
// FileName: MapRes.h
// Creater: ZhangXuedong
// Date: 2005.09.05
// Comment: Map Resource
//=============================================================================

#ifndef MAPRES_H
#define MAPRES_H

// tchar.h нужен только на Windows; TCHAR даёт словарь совместимости.
#include "PlatformCompat.h"
#include <stdio.h>
#include "World/TerrainAttrib.h"
#include "World/BlockData.h"
#include "point.h"
#include "Core/Timer.h"
#include "App/Config.h"
#include <fstream>

class CChaSpawn;
class CMapSwitchEntitySpawn;
class CNpcSpawn;
class SubMap;

using namespace Corsairs::Util;

namespace Corsairs::Common::Mission
{
	class CNpc;
}

class CAreaData
{
public:
	CAreaData();
	~CAreaData();
	std::int32_t	Init(_TCHAR *chFile);
	void	Free();
	bool	GetUnitAttr(std::int16_t sUnitX, std::int16_t sUnitY, std::uint16_t &usAttribute);
	bool	GetUnitSize(std::int16_t *psWidth, std::int16_t *psHeight);
	bool	GetUnitIsland(std::int16_t sUnitX, std::int16_t sUnitY, std::uint8_t &uchIsland);
	std::int16_t	GetWidth();
	std::int16_t	GetHeight();
	bool	    IsValidPos(std::int16_t sUnitX, std::int16_t sUnitY);

protected:

private:
	std::int16_t	m_sUnitCountX;	// 
	std::int16_t	m_sUnitCountY;	// 
	std::int16_t	m_sUnitWidth;	// 
	std::int16_t	m_sUnitHeight;	// 

	int			m_nID;

};

#define	defMAX_MAP_COPY_NUM	3000
#define defMAPCOPY_INFO_PARAM_NUM	16


enum EMapEntryStep
{
	enumMAPENTRY_CREATE,	// 
	enumMAPENTRY_DESTROY,	// 
	enumMAPENTRY_SUBPLAYER,	// 
	enumMAPENTRY_SUBCOPY,	// 
	enumMAPENTRY_RETURN,	// 
	enumMAPENTRY_COPYPARAM,	// 
	enumMAPENTRY_COPYRUN,	// 
};

enum EMapEntryOptRet
{
	enumMAPENTRYO_CREATE_SUC,		// 
	enumMAPENTRYO_DESTROY_SUC,		// 
	enumMAPENTRYO_COPY_CLOSE_SUC,	// 
};

enum EMapState
{
	enumMAP_STATE_OPEN,			// 
	enumMAP_STATE_CLOSE,		// 
	enumMAP_STATE_ASK_CLOSE,	// 
};

enum EMapEntryState
{
	enumMAPENTRY_STATE_ASK_OPEN,	// 
	enumMAPENTRY_STATE_OPEN,		// 
	enumMAPENTRY_STATE_CLOSE,		// 
	enumMAPENTRY_STATE_ASK_CLOSE,	// 
	enumMAPENTRY_STATE_CLOSE_SUC,	// 
};

enum EMapType // 
{
	enumMAPTYPE_NORMAL			= 1, // 
	enumMAPTYPE_GUILD_FIGHT		= 2, // 
	enumMAPTYPE_TEAM_FIGHT		= 3, // 
};

enum EMapCopyStartType // 
{
	enumMAPCOPY_START_NOW		= 1, // 
	enumMAPCOPY_START_PLAYER	= 2, // 
	enumMAPCOPY_START_CONDITION	= 3, // 
};

enum EMapCopyStartCdtType // 
{
	enumMAPCOPY_START_CDT_UNKN,		// 
	enumMAPCOPY_START_CDT_PLYNUM,	// 
};

class CMapRes
{
public:
	enum{m_eyeshotwidth = 2};

	CMapRes();
	virtual ~CMapRes();

	bool		Init(void);
	bool		IsValid(void);
	bool		IsOpen(void);
	bool		SetCopyNum(std::int16_t sCpyNum);
	std::int16_t	GetCopyNum(void);
	SubMap*		GetCopy(std::int16_t sCpyNO = -1);
	void		SetCopyPlyNum(std::int16_t sPlyNum);
	std::int16_t	GetCopyPlyNum(void);
	bool		InitCtrl(void);

	bool		Open(void);
	bool		Close(void);
	bool		OpenEntry(void);
	bool		CloseEntry(void);
	bool		CreateEntry(void);
	bool		DestroyEntry(void);
	void		Run(DWORD dwCurTime);
	bool		CopyClose(std::int16_t sCopyNO = -1);
	bool		CopyNotice(const char *szString, std::int16_t sCopyNO = -1);
	bool		ReleaseCopy(std::int16_t sCopyNO = 0);

	bool		SetEntryMapName(const char *szMapName);
	void		CheckEntryState(char chState);
	bool		SubEntryPlayer(std::int16_t sCopyNO);
	bool		SubEntryCopy(std::int16_t sCopyNO);
	bool		HasDynEntry(void);
	void		SetCanSavePos(bool bCan = true);
	bool		CanSavePos(void);
	void		SetCanPK(bool bCan = true);
	bool		CanPK(void);
	void		SetCanTeam(bool bCan = true);
	void		SetCanStall(bool bCan = true);
	void		SetCanGuild(bool bCan = true);
	void		SetGuildWar(bool bGuildWar);
	bool		CanGuildWar();
	bool		CanTeam(void);
	bool		CanStall(void);
	bool		CanGuild(void);
	void		SetType(char chType = enumMAPTYPE_NORMAL);
	char	GetType(void);
	void		SetCopyStartType(char chStartType = enumMAPCOPY_START_PLAYER);
	char	GetCopyStartType(void);
	void		SetCopyStartCondition(char chType, std::int32_t lVal);
	char	GetCopyStartCdtType(void);
	std::int32_t	GetCopyStartCdtVal(void);

	void		SetName(const char *cszName);
	const char*	GetName(void);
	const		Rect&	GetRange(void);
	BYTE		GetMapID();

	BOOL		SummonNpc( USHORT sAreaID, const char szNpc[], USHORT sTime );
	void		SetRepatriateDie(bool bRepatriate = true);
	bool		IsRepatriateDie(void);

	void		BeginGetUsedCopy(void);
	SubMap*		GetNextUsedCopy(void);

	Corsairs::Common::Mission::CNpc*		FindNpc( const char szName[] );

	// 
	struct
	{
		const std::int16_t		m_csEyeshotCellWidth;
		const std::int16_t		m_csEyeshotCellHeight;
		std::int16_t		m_sEyeshotCellLin;
		std::int16_t		m_sEyeshotCellCol;
	};
	// 
	struct
	{
		const std::int16_t		m_csStateCellWidth;
		const std::int16_t		m_csStateCellHeight;
		std::int16_t		m_sStateCellLin;
		std::int16_t		m_sStateCellCol;
	};

	CAreaData			m_CTerrain;
	struct
	{
		CBlockData		m_CBlock;
		const std::int16_t		m_csBlockUnitWidth;
		const std::int16_t		m_csBlockUnitHeight;
	};

	struct
	{
		std::string	m_strMapName;
		Rect			m_SRange;
		CMapRes			*m_pCLeftMap, *m_pCTopMap, *m_pCRightMap, *m_pCBelowMap;
	};

	CChaSpawn				*m_pCMonsterSpawn;
	CMapSwitchEntitySpawn	*m_pCMapSwitchEntitySpawn;
	CNpcSpawn*				m_pNpcSpawn;

	// 
	struct
	{
		char	m_szEntryMapName[MAX_MAPNAME_LENGTH];
		Point		m_SEntryPos;
		char	m_chEntryState;		// EMapEntryState

		time_t		m_tEntryFirstTm;	// 
		time_t		m_tEntryTmDis;		// 
		time_t		m_tEntryOutTmDis;	// 
		time_t		m_tMapClsTmDis;		// 

		FILE		*m_pfEntryFile;			// 
	};

	struct{
		char m_szObstacleFile[_MAX_PATH + _MAX_FNAME];
		char m_szSectionFile[_MAX_PATH + _MAX_FNAME];
		char m_szMonsterSpawnFile[_MAX_PATH + _MAX_FNAME];
		char m_szNpcSpawnFile[_MAX_PATH + _MAX_FNAME];
		char m_szMapSwitchFile[_MAX_PATH + _MAX_FNAME];
		char m_szMonsterCofFile[_MAX_PATH + _MAX_FNAME];
		char m_szCtrlFile[_MAX_PATH + _MAX_FNAME];
		char m_szEntryFile[_MAX_PATH + _MAX_FNAME];
	};

protected:

private:
	bool		m_bValid;	// 
	char	m_chState;	// EMapState

	BYTE	m_byMapID; // ID

	struct
	{
		bool	m_bCanSavePos;	// 
		bool	m_bCanPK;		// PK
		bool	m_bCanTeam;		// 
		bool	m_bCanStall;	// 
		bool	m_bCanGuild;
	};

	CTimer	m_timeMgr;
	CTimer	m_timeRun;

	SubMap		*m_pCMapCopy;
	std::int16_t	m_sMapCpyNum;
	std::int16_t	m_sCopyPlyNum;
	char	m_chType;

	struct
	{
		char	m_chCopyStartType;		//  EMapCopyStartType
		char	m_chCopyStartCdtType;	// 
		std::int32_t	m_lCopyStartCdtVal;	//
	};

	bool	m_bRepatriateDie;	// 

	std::int16_t	m_sUsedCopySearch;

	bool		m_bGuildWar;

};

// IDID,
class CMapID
{
public:
	CMapID();
	~CMapID();
	
	void	Clear();

	BOOL	AddInfo( const char szMap[], BYTE byID );
	BOOL	GetID( const char szMap[], BYTE& byID );	
	BOOL	SetMap( BYTE byID, CMapRes* pMap );
	CMapRes* GetMap( BYTE byID );

private:
	struct MAP_INFO
	{
		char		szMap[MAX_MAPNAME_LENGTH];
		BYTE		byID;
		CMapRes*	pMap;
	};

	std::vector<MAP_INFO> m_MapInfo;
	BYTE	 m_byNumMap;
};

extern CMapID g_MapID;
extern const char g_cchLogMapEntry;

#endif
