//=============================================================================
// FileName: GameApp.h
// Creater: ZhangXuedong
// Date: 2004.11.04
// Comment: CChaMgr class
//=============================================================================

#ifndef GAMEAPP_H
#define GAMEAPP_H


//#define DISABLE_GM_CMD				//	gm


#include "App/GameAppNet.h"
#include "TrackedPool.h"
#include "point.h"
#include "Services/Mission/RoleData.h"
#include "App/Config.h"
#include "Core/Identity.h"
#include "Player/Player.h"
#include "Skill/SkillStateRecord.h"
#include "Skill/SkillStateRecordStore.h"
#include "Script/Script.h"
#include "World/AreaRecord.h"
#include "Combat/SkillTemp.h"
#include "World/StateCell.h"
#include "Script/LuaAPI.h"
#include "Core/CommFunc.h"
#include "Entity/GamePool.h"
#include "World/MapRes.h"
#include "Services/Picture/PicSet.h"

#define defMAX_CHARINFO_NO 2500

class CCharacter;
class CPlayer;
class CItem;
class CPassengerMgr;

namespace Corsairs::Common::Mission
{
 class CTalkNpc;
 class CNpc;
 class CTradeData;
 class CStallData;
}

class SubMap;
class CCharacter;
namespace Corsairs::Common::Item { class CItemRecordAttr; }
using CItemRecordAttr = Corsairs::Common::Item::CItemRecordAttr;

class GateServer;


#define MAX_DBLOG_POOL 100

struct SDBLogData
{
	int		nLoc;			// pool
	char	szLog[8192];	//
	SDBLogData():nLoc(0) {}
};


class CDBLogMgr // dblog
{

public:

	CDBLogMgr();

	// Log 5, 8000
	void Log(const char *type, const char *c1, const char *c2, const char *c3, const char *c4, const char *p, BOOL bAddToList = TRUE);

	// Add by lark.li 20080324 begin
	void TradeLog(const char* action, const char *pszChaFrom, const char *pszChaTo, const char *pszTrade);
	// End

	void HandleLogList();
	void FlushLogList();  // GameServer, logDB

	int  GetLogLeft();
	int  SetPerLogCnt(int nCnt);
	int  GetPerLogCnt();

protected:

	std::list<SDBLogData*>	_LogList;
	int					_nPerLogCnt;				// log
	int					_nLogLeft;					// log
	SDBLogData			_LogPool[MAX_DBLOG_POOL];	// log, pool
	int					_nPoolUseLoc;				// pool
};

struct SVolunteer
{
	char szName[defENTITY_NAME_LEN];	//
	unsigned long ulID;					// ID
	long lLevel;						//
	long lJob;							//
	char szMapName[256];				//
};

class	CGameApp : public CDBLogMgr
{
public:

    CGameApp();
	~CGameApp();

	BOOL	Init();
	BOOL    InitMap();
    void	Run(DWORD dwCurTime);

	// 4
	void	ProcessNetMsg(int nMsgType, GateServer *pGate, Corsairs::Net::RPacket& pkt);
	void    OnGateConnected(GateServer *pGate, Corsairs::Net::RPacket& pkt);
	void	OnGateDisconnect(GateServer *pGate, Corsairs::Net::RPacket& pkt);
	void	ProcessPacket(GateServer* pGate, Corsairs::Net::RPacket& pkt);
	void	ProcessTeamMsg(GateServer *pGate, const Corsairs::Net::Msg::PmTeamMessage& teamMsg);
	void	ProcessGuildMsg(GateServer *pGate, const Corsairs::Net::Msg::PmGuildInfoMessage& msg);
	void	ProcessGuildChallMoney(GateServer *pGate, const Corsairs::Net::Msg::PmGuildChallMoneyMessage& msg);
	void	ProcessGuildChallPrizeMoney(GateServer *pGate, const Corsairs::Net::Msg::PmGuildChallPrizeMoneyMessage& msg);
	void	ProcessDynMapEntry(GateServer *pGate, const Corsairs::Net::Msg::MapEntryMessage& mapMsg);
	void	ProcessInterGameMsg(unsigned short usCmd, GateServer *pGate, Corsairs::Net::RPacket& pkt);
	void	Handle_GuildApprove(const Corsairs::Net::Msg::MmGuildApproveMessage& msg);
	void	Handle_CallCha(const Corsairs::Net::Msg::MmCallChaMessage& msg);
	void	Handle_GotoCha(GateServer* pGate, long lGatePlayerID, long lGatePlayerAddr, const Corsairs::Net::Msg::MmGotoChaMessage& msg);
	void	Handle_KickCha(const Corsairs::Net::Msg::MmKickChaMessage& msg);
	void	Handle_GuildChallPrizeMoney(const Corsairs::Net::Msg::MmGuildChallPrizeMoneyMessage& msg);
	void	Handle_AddCredit(const Corsairs::Net::Msg::MmAddCreditMessage& msg);
	void	Handle_StoreBuy(const Corsairs::Net::Msg::MmStoreBuyMessage& msg);
	void	Handle_AddMoney(const Corsairs::Net::Msg::MmAddMoneyMessage& msg);
	void	ProcessGroupBroadcast(unsigned short usCmd, GateServer *pGate, Corsairs::Net::RPacket& pkt);
	void	ProcessGarner2Update(const Corsairs::Net::Msg::PmGarner2UpdateLegacyMessage& msg);

	//------------------------------------


    CPlayer*    CreateGamePlayer(const char szPassword[], std::uint32_t ulChaDBId, std::uint32_t ulWorldId, const char *cszMapName, char chType);
    void        ReleaseGamePlayer(CPlayer*);
	void		GoOutGame(CPlayer* pPlayer, bool bOffLine, bool mOffLine = false);
	CPlayer*	GetNewPlayer();
	CPlayer*	GetPlayer(long lHandle);
	CPlayer*	IsValidPlayer(long lID, long lHandle);
    CCharacter* GetNewCharacter();
	CItem*		GetNewItem();
	Corsairs::Common::Mission::CTalkNpc*	GetNewTNpc();
	Entity*		GetEntity(long lHandle);
	Entity*		IsValidEntity(unsigned long ulID, long lHandle);
	Entity*		IsLiveingEntity(unsigned long ulID, long lHandle);
	Entity*		IsMapEntity(unsigned long ulID, long lHandle);
	Entity*		IsLifeEntity(unsigned long ulID, long lHandle);
	void		AddPlayerIdx(DWORD dwDBID, CPlayer* pPlayer);
	void		DelPlayerIdx(DWORD dwDBID);
	CPlayer*    GetPlayerByDBID(DWORD dwDBID);
	CPlayer*	GetPlayerByMainChaName(	const	char*	sMainChaName	);
	void		AfterPlayerLogin(const char *cszPlyName);
	void		NoticePlayerLogin(CPlayer *pCPlayer);

	CMapRes*	GetMap(int no);
	CMapRes*	FindMapByName(const char *mapname, bool bIncUnRun = false);
	void		LoadAllTable(void);
	void		LoadCharacterInfo(void);
	void		LoadSkillInfo(void);
	void		LoadItemInfo(void);
	void		SetGlobalRates(float droprate, float exprate);
	float		GetGlobalDropRate();
	float		GetGlobalExpRate();
	// npc
	BOOL		ReloadNpcInfo( CCharacter& character );
	Corsairs::Common::Mission::CNpc* FindNpc( const char szName[] );

	// NPC
	BOOL		SummonNpc( BYTE byMapID, USHORT sAreaID, const char szNpc[], USHORT sTime );

	//
	Corsairs::Common::Mission::CEventEntity* CreateEntity( BYTE byType );

	void		NotiGameReset(unsigned long ulLeftSec);
	void		SaveAllPlayer(void);

	void		SetEntityEnableLog(bool bValid = true);
	CSkillTempData*	GetSkillTData(short sSkillNo, char chSkillLv);
	void		SetSkillTDataRange(short *psRange);
	void		SetSkillTDataState(short *psState);
	void		InitSStateTraOnTime();
	long		GetSStateTraOnTime(unsigned char uchStateID, unsigned char uchStateLv);

	void		DataStatistic(void); //
	CCharacter*	FindPlayerChaByName(const char* cszChaName);
	CCharacter*	FindPlayerChaByNameLua(const char* cszChaName);
	int FindPlayerChaByActNameLua(const char* cszChaName,CCharacter* chas[3]);
	bool		DealAllInGuild(int guildID, const char* luaFunc, const char* luaParam);
	CCharacter*	FindPlayerChaByID(unsigned long ulChaID);
	CCharacter* FindMainPlayerChaByID(unsigned long ulChaID);
	CCharacter*	FindChaByID(unsigned long ulChaID);
	CCharacter*	FindChaByName(const char* cszChaName);
	CPlayer*	FindPlayerByDBChaID(unsigned long ulDBChaID);

	void		WorldNotice(const char *szString); // GameServer
	void		GuildNotice(unsigned long guildID, const char *szString);
	void		ScrollNotice(const char * szString,int SetNum, DWORD color = 0xFFFFFF00);//Add by sunny.sun20080804
	void		GMNotice(const char * szString);//add by sunny.sun 20080821
	void		LocalNotice(const char *szString); // GameServer
	void		ChaNotice(const char *szNotiString, const char *szChaName = ""); //

	bool		IsChaAttrMaxValInit(void);
	void		ChaAttrMaxValInit(bool bSet);

	void		CheckSeeWithTeamChange(bool CanSeen[][2], CPlayer **pCPlayerList, char chMemberCnt);
	void		RefreshTeamEyeshot(bool CanSeenOld[][2], bool CanSeenNew[][2], CPlayer **pCPlayerList, char chMemberCnt, char chRefType);
	void		RefreshTeamEyeshot(CPlayer **pCPlayerList, char chMemberCnt, char chRefType);

	BOOL		AddVolunteer(CCharacter *pCha);
	BOOL		DelVolunteer(CCharacter *pCha);
	SVolunteer	*GetVolInfo(int nIndex);
	int			GetVolNum();
	SVolunteer	*FindVolunteer(const char *szName);

	void		BanAccount(const char *szString);
	void		UnbanAccount(const char *szString);
	void		CanReceiveRequests(std::uint32_t chaID, bool CanSend);
	short		GetMapNum();

	DWORD   m_dwFPS;
	DWORD   m_dwRunCnt;
	DWORD	m_dwChaCnt;
	DWORD	m_dwPlayerCnt;
	DWORD	m_dwActiveMgrUnit;
	std::atomic<LONG>   m_dwRunStep;	//

	BOOL	m_bExecLuaCmd;
	std::string	m_strMapNameList;

	Corsairs::Util::TrackedPool<CPassengerMgr>				m_CabinPool{"Cabin"};
	Corsairs::Util::TrackedPool<Corsairs::Common::Mission::CTradeData>		m_TradeDataPool{"TradeData"};
	Corsairs::Util::TrackedPool<Corsairs::Common::Mission::CStallData>		m_StallDataPool{"StallData"};
	Corsairs::Util::TrackedPool<CSkillTempData>				m_SkillTDataPool{"SkillTData"};
	Corsairs::Util::TrackedPool<CStateCell>					m_MapStateCellPool{"StateCell"};
	Corsairs::Util::TrackedPool<CChaListNode>				m_ChaListPool{"ChaList"};
	Corsairs::Util::TrackedPool<CStateCellNode>				m_StateCellNodePool{"StateCellNode"};

	//
	std::int32_t	m_lCabinHeapNum;
	std::int32_t	m_lTradeDataHeapNum;
	std::int32_t	m_lSkillTDataHeapNum;
	std::int32_t	m_lMapMgrUnitHeapNum;
	std::int32_t	m_lEntityListHeapNum;
	std::int32_t	m_lMgrNodeHeapNum;
	//

	Identity			m_Ident;
	Identity			m_ItemIdent;

	struct // GameServer
	{
		unsigned long	m_ulLeftSec;
	    CTimer			m_CTimerReset;
	};

protected:
	void	MgrUnitRun(DWORD dwCurTime);
	void	GameItemRun(DWORD dwCurTime);
	void	MapMgrRun(DWORD dwCurTime);

protected:
	std::vector<CMapRes*> m_MapList;
	short	 m_mapnum;

	std::map<DWORD, CPlayer*>  _PlayerIdx;  // DB IDPlayer

	std::vector<SVolunteer>	m_vecVolunteerList;	//

	struct
	{
		CPlayer		*pCPlayerL;	  // socket
		CPlayer		*pCCurPlayer; //
	} m_GatePlayer[MAX_GATE];

public:
	CPicSet			   *m_PicSet;


private:

	DWORD   _dwFPS;
    DWORD   _dwLastTick;
    DWORD   _dwRunCnt;
    DWORD   _dwTempRunCnt;
	float   m_fGlobalDropRate;
	float   m_fGlobalExpRate;
	//
	struct
	{
		CSkillTempData	*m_pCSkillTData[defMAX_SKILL_NO + 1][defMAX_SKILL_LV + 1];
		short			m_sSkillSetNo;
		char			m_chSkillSetLv;
	};

	long	m_lSStateTraOnTime[AREA_STATE_MAXID + 1][SKILL_STATE_LEVEL + 1];	//

    CTimer	m_CTimerItem;

	bool	m_bChaAttrMaxValInit;
};

enum EChaTimerAction
{
	enumCHA_TIMEER_ENTERMAP,
};


struct SSwitchMapInfo //
{
	SubMap		*pSrcMap;
	char		szSrcMapName[256];
	Corsairs::Util::Point		SSrcPos;
	char		szTarMapName[256];
	Corsairs::Util::Point		STarPos;
};

extern bool             g_bLogEntity;

extern CGameApp*        g_pGameApp;
extern std::unordered_map<int, CItemRecordAttr> g_itemAttrMap;
extern CCharacter*		g_pCSystemCha;		//
extern SubMap *			g_pScriptMap;		//
extern std::string			g_strChaState[2];	// 01
extern std::uint32_t			g_ulCurID;
extern std::int32_t				g_lCurHandle;
extern HANDLE			hConsole;

// __VA_OPT__ убирает запятую, когда переменных аргументов нет: запись
// printf(s, __VA_ARGS__) при вызове C_PRINT("текст") разворачивается в
// printf(s, ) — MSVC это проглатывал, clang считает синтаксической ошибкой.
#define C_PRINT(s, ...) \
	SetConsoleTextAttribute(hConsole, 14); \
	printf(s __VA_OPT__(,) __VA_ARGS__); \
	SetConsoleTextAttribute(hConsole, 10);

#define C_TITLE(s) \
	char szPID[32]; \
	_snprintf_s(szPID,sizeof(szPID),_TRUNCATE, "%d", GetCurrentProcessId()); \
	std::string strConsoleT; \
	strConsoleT += "[PID:"; \
	strConsoleT += szPID; \
	strConsoleT += "]"; \
	strConsoleT += s; \
	SetConsoleTitle(strConsoleT.c_str());

#define defINVALID_CHA_ID		0
#define defINVALID_CHA_HANDLE	-1

#endif // GAMEAPP_H
