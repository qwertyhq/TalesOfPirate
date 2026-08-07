//=============================================================================
// FileName: Character.h
// Creater: ZhangXuedong
// Date: 2004.10.19
// Comment: CCharacter class
//=============================================================================

#ifndef CHARACTER_H
#define CHARACTER_H

#include "Combat/MoveAble.h"
#include "Network/NetCommand.h"
#include "Network/NetRetCode.h"
#include "Core/RoleCommon.h"
#include "Network/CompCommand.h"
#include "Core/CommFunc.h"
#include "Services/Mission/Mission.h"
#include "Core/Timer.h"
#include "Inventory/Kitbag.h"
#include "Inventory/ShipSet.h"
#include "Combat/Action.h"
#include "Character/ChaAction.h"

#include <array>
#include <utility>

// Краткий доступ к типам ChaAction.h на уровне файла — чтобы колсайты
// CCharacter могли писать ActControl::MOVE вместо полного квалификатора.
using ActControl = Corsairs::Common::Character::ActControl;
inline constexpr auto kActControlCount = Corsairs::Common::Character::kActControlCount;

#define defMAX_ITEM_NUM			10
#define defCHA_SCRIPT_TIMER		1 * 1000
#define defDEFAULT_PING_VAL		500
#define defPING_RECORD_NUM		6
#define defPING_INTERVAL		20 * 1000
#define defCHA_SCRIPT_PARAM_NUM	1

extern CCharacter*		g_pCSystemCha;		// 

struct SLean // 
{
	std::uint32_t	ulPacketID; // ID
	char	chState;	// 0.1
	std::int32_t	lPose;
	std::int32_t	lAngle;
	std::int32_t	lPosX, lPosY;
	std::int32_t	lHeight;
};

struct SSeat
{
	char	chIsSeat;
	int16_t	sAngle;
	int16_t	sPose;
};

struct STempChaPart
{
	short	sPartID;
	short	sItemID;
};

struct SCheatX
{
	std::uint32_t Xtype;			//1: 2:
	std::uint32_t Xerror;		//
	std::uint32_t Xright;		//
	std::uint32_t Xcount;		//
	std::uint32_t Xn;
	DWORD dwLastTime;	//
	DWORD dwInterval;	//
	std::string Xnum;		//X number
};

enum ESwitchMapType
{
	enumSWITCHMAP_CARRY,	// 
	enumSWITCHMAP_DIE,		// 
};

enum ELogAssetsType	// 
{
	enumLASSETS_INIT,		// 
	enumLASSETS_TRADE,		// 
	enumLASSETS_BANK,		// 
	enumLASSETS_PICKUP,		// 
	enumLASSETS_THROW,		// 
	enumLASSETS_DELETE,		// 
};

namespace Corsairs::Common::Mission
{
	class CStallData;
	class CTradeData;
	class CTalkNpc;
}

#define LOOK_SELF   0
#define LOOK_OTHER  1
#define LOOK_TEAM   2

class CHateMgr;
class CAction;
class CActionCache;

class CCharacter : public CMoveAble
{
	friend class CChaSpawn;
	friend class PlayerStorage;
	friend class Guild;
	friend class CTableGuild;
public:
	CCharacter();
	~CCharacter();

	void	Initially();
	void	Finally();

	bool	IsPlayerCha(void); //
	bool	IsGMCha(); // GM0-10GM
	bool	IsGMCha2(); // GM

	// Режим разработчика — прогон мира без оглядки на баланс. Включается
	// GM-командой `dev`, живёт только в памяти и в базу не сохраняется:
	// перезаход возвращает персонажа к обычным характеристикам.
	bool	IsDevInvincible() const { return _devInvincible; }
	void	SetDevMode(bool bEnable);
	void	SetDevSpeed(std::int32_t lSpeed);
	bool	IsPlayerCtrlCha(void); // 
	bool	IsPlayerMainCha(void); // 
	bool	IsPlayerFocusCha(void); //  IsPlayerCtrlCha
	bool	IsPlayerOwnCha(void); // 
	CCharacter	*GetPlyCtrlCha(void);
	CCharacter	*GetPlyMainCha(void);

	void		SetIMP(int x, bool sync = true);
	int			GetIMP();

	float		GetDropRate();
	float       GetExpRate();

	void ItemUnlockRequest(const Corsairs::Net::Msg::CmItemUnlockAskMessage& msg);

	void	WriteInt64PartInfo(Corsairs::Net::WPacket& packet);
	void	SwitchMap(SubMap *pCSrcMap, const char *szTarMapName, std::int32_t lTarX, std::int32_t lTarY, bool bNeedOutSrcMap = true, char chSwitchType = enumSWITCHMAP_CARRY, std::int32_t lTMapCpyNO = -1);

	virtual void	ProcessPacket(std::uint16_t usCmd, Corsairs::Net::RPacket& pk);

	//    (  ProcessPacket) 
	void Handle_GuildBankCmd(const Corsairs::Net::Msg::PmGuildBankMessage& msg);
	void Handle_PushToGuildBank(const std::string& strItem);
	void Handle_Ping(const Corsairs::Net::Msg::CmPingResponseMessage& msg);
	void Handle_Say(const Corsairs::Net::Msg::CmSayMessage& msg);
	void Handle_RequestTalk(std::uint32_t npcId, Corsairs::Net::RPacket& pk);
	void Handle_DailyBuffRequest();
	void Handle_RefreshData(const Corsairs::Net::Msg::CmRefreshDataMessage& msg);
	void Handle_MapMask();
	void Handle_ItemForgeAsk(const Corsairs::Net::Msg::CmItemForgeGroupAskMessage& msg);
	void Handle_ItemLotteryAsk(const Corsairs::Net::Msg::CmItemLotteryGroupAskMessage& msg);
	void Handle_LifeSkillAsk(const Corsairs::Net::Msg::CmLifeSkillCraftMessage& msg);
	void Handle_LifeSkillAsr(const Corsairs::Net::Msg::CmLifeSkillCraftMessage& msg);
	void Handle_StoreCommand(std::uint16_t usCmd, Corsairs::Net::RPacket& pk);
	void Handle_TigerStart(const Corsairs::Net::Msg::CmTigerStartMessage& msg);
	void Handle_TigerStop(const Corsairs::Net::Msg::CmTigerStopMessage& msg);
	void Handle_VolunteerOpen(const Corsairs::Net::Msg::CmVolunteerOpenMessage& msg);
	void Handle_VolunteerList(const Corsairs::Net::Msg::CmVolunteerListMessage& msg);
	void Handle_VolunteerSel(const Corsairs::Net::Msg::CmVolunteerSelMessage& msg);
	void Handle_VolunteerAsr(const Corsairs::Net::Msg::CmVolunteerAsrMessage& msg);
	void Handle_KitbagTempSync();
	void Handle_ItemLockAsk(const Corsairs::Net::Msg::CmItemLockAskMessage& msg);
	void Handle_GameRequestPin(const Corsairs::Net::Msg::CmGameRequestPinMessage& msg);
	void Handle_MasterInvite(const Corsairs::Net::Msg::CmMasterInviteMessage& msg);
	void Handle_MasterAsr(const Corsairs::Net::Msg::CmMasterAsrMessage& msg);
	void Handle_MasterDel(const Corsairs::Net::Msg::CmMasterDelMessage& msg);
	void Handle_PrenticeDel(const Corsairs::Net::Msg::CmPrenticeDelMessage& msg);

	virtual void	Run(std::uint32_t ulCurTick);
	virtual	void	RunEnd( DWORD dwCurTime );
	virtual void	OnScriptTimer(DWORD dwExecTime, bool bNotice = false);
	virtual void	StartExit();
	virtual void	CancelExit();
	virtual void	Exit();

	void	CheatRun(DWORD dwCurTime);
	void	CheatCheck(const char *answer);
	void	CheatConfirm();
	void	InitCheatX();
	DWORD	GetCheatInterval(int state);

	// 
	bool		Cmd_EnterMap(const char* l_map, std::int32_t lMapCopyNO, std::uint32_t l_x, std::uint32_t l_y, char chLogin = 1);
	void		Cmd_BeginMove(int16_t sPing, Corsairs::Util::Point *pPath, char chPointNum, char chStopState = enumEXISTS_WAITING);
	void		Cmd_BeginMoveDirect(Entity *pTar);
	void		Cmd_BeginSkill(int16_t sPing, Corsairs::Util::Point *pPath, char chPointNum,
				CSkillRecord *pSkill, std::int32_t lSkillLv, std::int32_t lTarInfo1, std::int32_t lTarInfo2, char chStopState = enumEXISTS_WAITING);
	void		Cmd_BeginSkillDirect(std::int32_t lSkillNo, Entity *pTar, bool bIntelligent = true);
	void		Cmd_BeginSkillDirect2(std::int32_t lSkillNo, std::int32_t lSkillLv, std::int32_t lPosX, std::int32_t lPosY);
	int16_t	Cmd_UseItem(int16_t sSrcKbPage, int16_t sSrcKbGrid, int16_t sTarKbPage, int16_t sTarKbGrid);
	int16_t	Cmd_UseEquipItem(int16_t sKbPage, int16_t sKbGrid, bool bRefresh = true,bool rightHand = false);
	int16_t	Cmd_UseExpendItem(int16_t sSrcKbPage, int16_t sSrcKbGrid, int16_t sTarKbPage, int16_t sTarKbGrid, bool bRefresh = true);
	int16_t	Cmd_UnfixItem(char chLinkID, int16_t *psItemNum, char chDir, std::int32_t lParam1, std::int32_t lParam2, bool bPriority = true, bool bRefresh = true, bool bForcible = false);
	int16_t	Cmd_PickupItem(std::uint32_t ulID, std::int32_t lHandle);
	int16_t	Cmd_ThrowItem(int16_t sKbPage, int16_t sKbGrid, int16_t *psThrowNum, std::int32_t lPosX, std::int32_t lPosY, bool bRefresh = true, bool bForcible = false);
	int16_t	Cmd_ItemSwitchPos(int16_t sKbPage, int16_t sSrcGrid, int16_t sSrcNum, int16_t sTarGrid);
	int16_t	Cmd_DelItem(int16_t sKbPage, int16_t sKbGrid, int16_t *psThrowNum, bool bRefresh = true, bool bForcible = false);
	int16_t	Cmd_BankOper(char chSrcType, int16_t sSrcGridID, int16_t sSrcNum, char chTarType, int16_t sTarGridID);
	int16_t	Cmd_GuildBankOper(char chSrcType, int16_t sSrcGridID, int16_t sSrcNum, char chTarType, int16_t sTarGridID);
	
    //(sSrcGrid:   sSrcNum:   sTarGrid:)
    int16_t  Cmd_DragItem(int16_t sSrcGrid, int16_t sSrcNum, int16_t sTarGrid);
    
	void Cmd_SetInPK(bool bInPK = true);
	void Cmd_SetInGymkhana(bool bInGymkhana = true);
	void Cmd_SetPKGuild(bool v);

	void		Cmd_ReassignAttr(const Corsairs::Net::Msg::CmSynAttrMessage& msg);
	int16_t	Cmd_RemoveItem(std::int32_t lItemID, std::int32_t lItemNum, char chFromType, int16_t sFromID, char chToType, int16_t sToID, bool bRefresh = true, bool bForcible = true);

	void		Cmd_ChangeHair(Corsairs::Net::RPacket& pk);											// 
	void		Prl_ChangeHairResult(int nScriptID, const char* szReason, BOOL bNoticeAll = FALSE); // 
	void		Prl_OpenHair();															// 	

	void		Cmd_FightAsk(char chType, std::int32_t lTarID, std::int32_t lTarHandle);
	void		Cmd_FightAnswer(bool bFight);
	void		Cmd_ItemRepairAsk(char chPosType, char chPosID);
	void		Cmd_ItemRepairAnswer(bool bRepair);
	void		Cmd_ItemForgeAsk(char chType, SForgeItem *pSItem);
	void		Cmd_ItemForgeAnswer(bool bForge);

	// ADd by lark.li 20080515 begin
	void		Cmd_ItemLotteryAsk(SLotteryItem *pSItem);
	void		Cmd_ItemLotteryAnswer(bool bForge);
	// End
	
	//
	void		Cmd_Garner2_Reorder(short index);

	//
	void		Cmd_LifeSkillItemAsk(std::int32_t dwType, SLifeSkillItem *pSItem);
	void		Cmd_LifeSkillItemAsR(std::int32_t dwType,SLifeSkillItem *pSItem);
    //
    void        Cmd_LockKitbag();
    void        Cmd_UnlockKitbag(const char szPassword[]);
    void        Cmd_CheckKitbagState();
    void        Cmd_SetKitbagAutoLock(char cAuto);

	//
	BOOL		Cmd_AddVolunteer();
	BOOL		Cmd_DelVolunteer();
	void		Cmd_ListVolunteer(short sPage, short sNum);
	BOOL		Cmd_ApplyVolunteer(const char *szName);
	CCharacter	*FindVolunteer(const char *szName);
	
	virtual void	ReflectINFof(Entity *srcent, Corsairs::Net::WPacket chginf);
	virtual CCharacter *IsCharacter();





	void	TradeClear( CPlayer& player );
	bool	TradeAction(bool bLock = true);
	bool	StallAction(bool bLock = true);
	bool	HairAction(bool bLock = true);
	bool	RepairAction(bool bLock = true);
	bool	ForgeAction(bool bLock = true);

	void	BickerNotice( const char szData[], ... );
	void	SystemNotice( const char szData[], ... );
	void	PopupNotice( const char szData[], ... );
	void	ColourNotice( DWORD rgb, const char szData[], ... );
	bool	IsPKSilver();

	//
	void	SetTradeData( Corsairs::Common::Mission::CTradeData* pData );
	Corsairs::Common::Mission::CTradeData* GetTradeData();

	//
	void	SetBoat( CCharacter* pBoat );
	CCharacter* GetBoat();
	bool	IsBoat(void);
	
	// npc
	BOOL	SafeSale( BYTE byIndex, BYTE byCount, WORD& wItemID, DWORD& dwMoney );
	BOOL	SafeBuy( WORD wItemID, BYTE byCount, BYTE byIndex, DWORD& dwMoney );
	BOOL	SafeSaleGoods( DWORD dwBoatID, BYTE byIndex, BYTE byCount, WORD& wItemID, DWORD& dwMoney );
	BOOL	SafeBuyGoods( DWORD dwBoatID, WORD wItemID, BYTE byCount, BYTE byIndex, DWORD& dwMoney );
	BOOL	GetSaleGoodsItem( DWORD dwBoatID, BYTE byIndex, WORD& wItemID );

	BOOL	ExchangeReq(short sSrcID, short sSrcNum, short sTarID, short sTarNum);

	bool	SetNarmalSkillState(bool bAdd = true, std::uint8_t uchStateID = 1, std::uint8_t uchStateLv = 1);
	bool	HasTradeAction(void);
	//
	BOOL	SetMissionPage( DWORD dwNpcID, BYTE byPrev, BYTE byNext, BYTE byState );
	BOOL	GetMissionPage( DWORD dwNpcID, BYTE& byPrev, BYTE& byNext, BYTE& byState );
	BOOL	SetTempData( DWORD dwNpcID, WORD wID, BYTE byState, BYTE byType );
	BOOL	GetTempData( DWORD dwNpcID, WORD& wID, BYTE& byState, BYTE& byType );

	// 
	BOOL	SaveMissionData();	// 

	BOOL	AddMissionState( DWORD dwNpcID, BYTE byID, BYTE byState );
	BOOL	ResetMissionState( Corsairs::Common::Mission::CTalkNpc& npc );
	
	BOOL	GetMissionState( DWORD dwNpcID, BYTE& byState );
	BOOL	GetNumMission( DWORD dwNpcID, BYTE& byNum );
	BOOL	GetMissionInfo( DWORD dwNpcID, BYTE byIndex, BYTE& byID, BYTE& byState );
	BOOL	GetCharMission( DWORD dwNpcID, BYTE byID, BYTE& byState );
	BOOL	GetNextMission( DWORD dwNpcID, BYTE& byIndex, BYTE& byID, BYTE& byState );
	BOOL	ClearMissionState( DWORD dwNpcID );
	
	BOOL	AddTrigger( const Corsairs::Common::Mission::TRIGGER_DATA& Data );
	BOOL	ClearTrigger( WORD wTriggerID );
	BOOL	DeleteTrigger( WORD wTriggerID );
	BOOL	GetMisScriptID( WORD wID, WORD& wScriptID );
	BOOL	AddRole( WORD wID, WORD wParam );
	BOOL	HasRole( WORD wID );
	BOOL	ClearRole( WORD wID );
	BOOL	IsRoleFull();
	BOOL	SetFlag( WORD wID, WORD wFlag );
	BOOL	ClearFlag( WORD wID, WORD wFlag );
	BOOL	IsFlag( WORD wID, WORD wFlag );
	BOOL	IsValidFlag( WORD wFlag );
	BOOL	SetRecord( WORD wRec );
	BOOL	ClearRecord( WORD wRec );
	BOOL	IsRecord( WORD wRec );
	BOOL	IsValidRecord( WORD wRec );

	// 
	BOOL	HasRandMission( WORD wRoleID );
	BOOL	AddRandMission( WORD wRoleID, WORD wScriptID, MissionRandType byType, BYTE byLevel, DWORD dwExp, DWORD dwMoney, USHORT sPrizeData, USHORT sPrizeType, BYTE byNumData );
	BOOL	SetRandMissionData( WORD wRoleID, BYTE byIndex, const Corsairs::Common::Mission::MISSION_DATA& RandData );
	BOOL	GetRandMission( WORD wRoleID, MissionRandType& byType, BYTE& byLevel, DWORD& dwExp, DWORD& dwMoney, USHORT& sPrizeData, USHORT& sPrizeType, BYTE& byNumData );
	BOOL	GetRandMissionData( WORD wRoleID, BYTE byIndex, Corsairs::Common::Mission::MISSION_DATA& RandData );

	// npc(NPC)
	BOOL	HasSendNpcItemFlag( WORD wRoleID, WORD wNpcID );
	BOOL	NoSendNpcItemFlag( WORD wRoleID, WORD wNpcID );
	BOOL	HasRandMissionNpc( WORD wRoleID, WORD wNpcID, WORD wAreaID );

	// 
	BOOL	TakeRandNpcItem( WORD wRoleID, WORD wNpcID, const char szNpc[] );
	BOOL	TakeAllRandItem( WORD wRoleID );

	// 
	BOOL	IsMisNeedItem( USHORT sItemID );
	BOOL	GetMisNeedItemCount( WORD wRoleID, USHORT sItemID, USHORT& sCount );
	void	RefreshNeedItem( USHORT sItemID );

	// 
	void	MisLog();
	void	MisLogInfo( WORD wMisID );
	void	MisLogClear( WORD wMisID );

	// 
	BOOL	SetMissionComplete( WORD wRoleID );
	BOOL	SetMissionFailure( WORD wRoleID );
	BOOL	HasMissionFailure( WORD wRoleID );

	// 
	BOOL	CompleteRandMission( WORD wRoleID );
	BOOL	FailureRandMission( WORD wRoleID );
	BOOL	AddRandMissionNum( WORD wRoleID );
	BOOL	ResetRandMission( WORD wRoleID );
	BOOL	ResetRandMissionNum( WORD wRoleID );
	BOOL	HasRandMissionCount( WORD wRoleID, WORD wCount );
	BOOL	GetRandMissionCount( WORD wRoleID, WORD& wCount );
	BOOL	GetRandMissionNum( WORD wRoleID, WORD& wNum );

	// NPC
	BOOL	ConvoyNpc( WORD wRoleID, BYTE byIndex, WORD wNpcCharID, BYTE byAiType );
	BOOL	ClearConvoyNpc( WORD wRoleID, BYTE byIndex );
	BOOL	ClearAllConvoyNpc( WORD wRoleID );
	BOOL	HasConvoyNpc( WORD wRoleID, BYTE byIndex );
	BOOL	IsConvoyNpc( WORD wRoleID, BYTE byIndex, WORD wNpcCharID );

	// 
	void	AddMoney( const char szName[], DWORD dwMoney );
	BOOL	TakeMoney( const char szName[], DWORD dwMoney );
	BOOL	HasMoney( DWORD dwMoney );
	BOOL	AddItem( USHORT sItemID, USHORT sCount, const char szName[], BYTE byAddType = enumITEM_INST_TASK, BYTE bySoundType = enumSYN_KITBAG_FROM_NPC, BOOL isTradable = true, LONG expiration = 0, short* posID = NULL);
	BOOL	TakeItem( USHORT sItemID, USHORT sCount, const char szName[] );
	BOOL	GiveItem( USHORT sItemID, USHORT sCount, BYTE byAddType, BYTE bySoundType, BOOL isTradable = true, LONG expiration = 0, int16_t* posID = NULL);
	int		GiveItemReturnPosition(USHORT sItemID, USHORT sCount, BYTE byAddType, BYTE bySoundType);
	BOOL	MakeItem( USHORT sItemID, USHORT sCount, USHORT& sItemPos, BYTE byAddType = enumITEM_INST_TASK, BYTE bySoundType = enumSYN_KITBAG_FROM_NPC );
	BOOL	HasItem( USHORT sItemID, USHORT sCount );
	BOOL	GetNumItem( USHORT sItemID, USHORT& sCount );
	BOOL	HasLeaveBagGrid( USHORT sNum );
	BOOL	HasLeaveBagTempGrid( USHORT sNum );
	BOOL	HasItemBagTemp(USHORT sItemID, USHORT sCount);
	BOOL	TakeItemBagTemp(USHORT sItemID, USHORT sCount, const char szName[]);

	//  (   InfoNet.h)
	struct ItemInfo
	{
		short       itemID;
		unsigned char itemNum;
		unsigned char itemFlute;
		short       itemAttrID[5];
		short       itemAttrVal[5];
	};

	//
	BOOL	AddItem2KitbagTemp( USHORT sItemID, USHORT sCount, ItemInfo *pItemAttr, BYTE bySoundType = enumSYN_KITBAG_FROM_NPC );
	BOOL	AddItem2KitbagTemp( USHORT sItemID, USHORT sCount, const char szName[], BYTE byAddType = enumITEM_INST_TASK, BYTE bySoundType = enumSYN_KITBAG_FROM_NPC );
	BOOL	GiveItem2KitbagTemp( USHORT sItemID, USHORT sCount, ItemInfo *pItemAttr, BYTE bySoundType );
	BOOL	GiveItem2KitbagTemp( USHORT sItemID, USHORT sCount, BYTE byAddType, BYTE bySoundType );

	// 
	BOOL	SetProfession( BYTE byPf );

	bool	LearnSkill(int16_t sSkillID, char chLv, bool bSetLv = true, bool bUsePoint = true, bool bLimit = true); // 
	bool	AddSkillState(std::uint8_t uchFightID, std::uint32_t ulSrcWorldID, std::int32_t lSrcHandle, char chObjType, char chObjHabitat, char chEffType,
			std::uint8_t uchStateID, std::uint8_t uchStateLv, std::int32_t lOnTick, char chType = enumSSTATE_ADD_UNDEFINED, bool bNotice = true); //
	bool	DelSkillState(std::uint8_t uchStateID, bool bNotice = true); //

	//
	bool	GetActControl(Corsairs::Common::Character::ActControl ctrlType) const;
	void	Hide();
	void	Show();

	// 
	void	RestoreHp( BYTE byHpRate );
	void	RestoreSp( BYTE bySpRate );
	void	RestoreAllHp();
	void	RestoreAllSp();
	void	RestoreAll();

	BOOL	ViewItemInfo( const Corsairs::Net::Msg::CmActionViewItemData& msg );

	BOOL	AddAttr( int nIndex, DWORD dwValue, int16_t sNotiType = enumATTRSYN_TASK ); // ATTR_CEXPCFightAble::AddExp
	BOOL	TakeAttr( int nIndex, DWORD dwValue, int16_t sNotiType = enumATTRSYN_TASK );

	bool	    IsInPK(void);
	bool	    IsInGymkhana(void);
	void	    SetPKCtrl(char chCtrl);
	char   GetPKCtrl(void);
	bool	    CanPK(void);
	bool	    IsInArea(int16_t sAreaMask);
	void	SetRelive(char chType = enumEPLAYER_RELIVE_ORIGIN, char chLv = 0, const char *szInfo = 0);
	void	Reset(void);

	virtual void BreakAction(Corsairs::Net::RPacket* pk = nullptr);
	virtual void EndAction(Corsairs::Net::RPacket* pk = nullptr);
	// 
	virtual void AfterObjDie(CCharacter *pCAtk, CCharacter *pCDead);
	virtual void AfterPeekItem(int16_t sItemID, int16_t sNum);
	virtual void AfterEquipItem(int16_t sItemID, std::uint16_t sTriID);
	virtual void EntryMapUnit( BYTE byMapID, WORD wxPos, WORD wyPos );
	virtual void OnMissionTime(); // 
	virtual void OnLevelUp( USHORT sLevel );
	virtual void OnSailLvUp( USHORT sLevel );
	virtual void OnLifeLvUp( USHORT sLevel );
	virtual void OnCharBorn();

	// 
	BOOL	IsNeedRepair();
	BOOL	IsNeedSupply();
	void	RepairBoat();
	void	SupplyBoat();
	void	BoatDie( CCharacter& Attacker, CCharacter& Boat );
	BOOL	OnBoatDie( CCharacter& Attacker );
	BOOL	GetBoatID( BYTE byIndex, DWORD& dwBoatID );
	BOOL	BoatCreate( const BOAT_DATA& Data );
	BOOL	BoatUpdate( BYTE byIndex, const BOAT_DATA& Data );
	BOOL	BoatLoad( const BOAT_LOAD_INFO& Info );
	
	// 
	BOOL	AdjustTradeItemCess( USHORT sLowCess, USHORT sData );
	BOOL	GetTradeItemData( BYTE& byLevel, USHORT& sCess );
	BOOL	SetTradeItemLevel( BYTE byLevel );	
	BOOL	HasTradeItemLevel( BYTE byLevel );
	BOOL	GetTradeItemLevel( BYTE& byLevel );
	BOOL	BoatTrade( USHORT sBerthID );	

	BOOL	BoatBerth( USHORT sBerthID, USHORT sxPos, USHORT syPos, USHORT sDir );	
	BOOL	BoatLaunch( BYTE byIndex, USHORT sBerthID, USHORT sxPos, USHORT syPos, USHORT sDir );
	BOOL	BoatBerthList( DWORD dwNpcID, BoatListType byType, USHORT sBerthID, USHORT sxPos, USHORT syPos, USHORT sDir );
	BOOL	BoatSelLuanch( BYTE byIndex );
	BOOL	BoatSelected( BoatListType byType, BYTE byIndex );
	BOOL	HasAllBoatInBerth( USHORT sBerthID );
	BOOL	HasBoatInBerth( USHORT sBerthID );
	BOOL	HasDeadBoatInBerth( USHORT sBerthID );
	void	SetBoatLook( const stNetChangeChaPart& Info );
	BOOL	BoatPackBagList( USHORT sBerthID, BYTE byType, BYTE byLevel );
	BOOL	BoatPackBag( BYTE byIndex );
	BOOL	PackBag( CCharacter& boat, BYTE byType, BYTE byLevel );
	BOOL	PackBag( CCharacter& Boat, USHORT sItemID, USHORT sCount, USHORT sPileID, USHORT& sNumPack );
	void	SetBoatAttrChangeFlag(bool bSet = true);
	void	SyncBoatAttr( int16_t sSynType, bool bAllBoat = true ); // 

	// 
	BOOL	BoatAdd( CCharacter& Boat );
	BOOL	BoatClear( CCharacter& Boat );
	BOOL	BoatAdd( std::uint32_t dwDBID );
	BOOL	BoatClear( std::uint32_t dwDBID );

	// 
	BOOL	SetEntityState( DWORD dwEntityID, BYTE byState );
	void	SetEntityTime( DWORD dwTime );
	DWORD	GetEntityTime();

	// 
	BOOL	HasGuild();

	// 
	void	SetStallData( Corsairs::Common::Mission::CStallData* pData );
	Corsairs::Common::Mission::CStallData* GetStallData();
	BYTE	GetStallNum();

	//add by jilinlee 2007/4/20
	//
	BOOL IsReadBook();
	void SetReadBookState(bool bIsReadBook = false);


	// 
	void	ChangeItem(bool bEquip, SItemGrid *pItemCont, char chLinkID); // 
	void	SkillRefresh(); // 
	// 
	void	NewChaInit(void);
	// 
	void	ChaInitEquip(void);
	void	ResetBirthInfo(void);

	// 
	void	SynKitbagNew(char chType); // 
	void    SynKitbagTmpNew(char chType); // 
	void	SynShortcut(); // 
	void	SynLook(char chSynType = enumSYN_LOOK_SWITCH); // 
	void	SynLook(char chLookType, bool verbose);
	bool	ItemForge(SItemGrid *pItem, char chAddLv = 1); // 
	void	SynSkillBag(char chType); // 
	void	SynPKCtrl(void); // PK
	void	SynAddItemCha(CCharacter *pCItemCha);
	void	SynDelItemCha(CCharacter *pCItemCha);
	void	CheckPing(void);
	void	SendPreMoveTime(void);
	void	SynSideInfo(void);
	void	SynBeginItemRepair(void);
	void	SynBeginItemForge(void);
	
	void	SynBeginItemLottery(void);	//Add by lark.li 20080513

	void	SynBeginItemUnite(void);
	void	SynBeginItemMilling(void);
	void	SynBeginItemFusion();
	void    SynBeginItemUpgrade();
	void    SynBeginItemEidolonMetempsychosis();
	void	SynBeginItemEidolonFusion();
	void	SynBeginItemPurify();
	void	SynBeginItemFix();
	void	SynBeginItemEnergy();
	void	SynBeginGetStone();
	void	SynBeginTiger();
	void	SynAppendLook(void);
	void	SynItemUseSuc(int16_t sItemID);
	void	SynKitbagCapacity(void);
	void	SynEspeItem(void);
	void	SynVolunteerState(BOOL bState);
	void	SynTigerString(const char *szString);

	//

	// 
	void	WriteBaseInfo(Corsairs::Net::WPacket &pk, char chLookType = LOOK_SELF);
	void	WritePKCtrl(Corsairs::Net::WPacket &pk);
	void	WriteSkillbag(Corsairs::Net::WPacket &pk, int nSynType);
	void	WriteKitbag(CKitbag &CKb, Corsairs::Net::WPacket &pk, int nSynType);
	Corsairs::Net::Msg::ChaKitbagInfo BuildKitbagInfo(CKitbag &CKb, int nSynType);
	void	WriteLookData(Corsairs::Net::WPacket &pk, char chLookType = LOOK_SELF, char chSynType = enumSYN_LOOK_SWITCH);
	Corsairs::Net::Msg::ChaLookInfo BuildLookInfo(char chLookType = LOOK_SELF, char chSynType = enumSYN_LOOK_SWITCH);
	bool	WriteAppendLook(CKitbag &CKb, Corsairs::Net::WPacket &pk, bool bInit = false);
	void	WriteInt64cut(Corsairs::Net::WPacket &pk);
	void	WriteBoat(Corsairs::Net::WPacket &pk);
	void	WriteItemChaBoat(Corsairs::Net::WPacket &pk, CCharacter *pCBoat);
	void	WriteSideInfo(Corsairs::Net::WPacket &pk);
	// Fill*     (CommandMessages.h)
	void	FillBaseInfo(Corsairs::Net::Msg::ChaBaseInfo &b, char chLookType = LOOK_SELF);
	void	FillSkillBag(Corsairs::Net::Msg::ChaSkillBagInfo &s, int nSynType);
	void	FillKitbag(Corsairs::Net::Msg::ChaKitbagInfo &k, CKitbag &CKb, int nSynType);
	void	FillShortcut(Corsairs::Net::Msg::ChaShortcutInfo &s);
	void	FillBoats(std::vector<Corsairs::Net::Msg::BoatData> &boats);
	//

	// 
	void	FailedActionNoti(char chType, char chReason);
	// 
	void	TerminalMessage(std::int32_t lMessageID);
	// 
	void	ItemOprateFailed(int16_t sFailedID);

	void		SetMotto(const char *szMotto);
	const char*	GetMotto(void);
	void		SetIcon(std::uint16_t usIcon);
	std::uint16_t	GetIcon(void);
	void		SetGuildName(const char *szGuildName);
	const char*	GetGuildName(void);
	const char*	GetValidGuildName(void);
	void		SetGuildMotto(const char *szGuildMotto);
	const char*	GetGuildMotto(void);
	const char*	GetValidGuildMotto(void) ;
	void		SetGuildID( DWORD dwGuildID );
	DWORD		GetGuildID();
	DWORD		GetValidGuildID();
	void		SetGuildState( std::uint32_t lState );
	std::uint32_t		GetGuildState();
	void		SetEnterGymkhana(bool bEnter = true);
	void		SyncGuildInfo();
	void		SetStallName(const char *szStallName);
	const char*	GetStallName(void);
	void		SynStallName(void);

	void			AddBlockCnt();
	BYTE			GetBlockCnt();
	void			SetBlockCnt(BYTE cnt);

	virtual void	AfterAttrChange(int nIdx, std::int32_t lOldVal, std::int32_t lNewVal);
	virtual void	Die();	// 
	void			JustDie(CCharacter *pCSrcCha);	// 
	void			MoveCity(const char *szCityName, std::int32_t lMapCpyNO = -1, char chSwitchType = enumSWITCHMAP_CARRY);
	void			BackToCity(bool Die = false, const char *szCityName = 0, std::int32_t lMapCpyNO = -1, char chSwitchType = enumSWITCHMAP_DIE);
	void			BackToCityEx(bool Die = false, const char *szCityName = 0, std::int32_t lMapCpyNO = -1, char chSwitchType = enumSWITCHMAP_DIE);
	void			SetToMainCha(bool bBoatDie = false);
	bool			CanSeen(CCharacter *pCCha);
	bool			CanSeen(CCharacter *pCCha, bool bThisEyeshot, bool bThisNoHide, bool bThisNoShow);
	bool			IsHide();
	SItemGrid*		GetEquipItem(char chPart);
	DWORD			GetTeamID();
	bool			IsTeamLeader();
	std::int32_t		GetSideID();
	void			SetSideID(std::int32_t lSideID);
	SItemGrid*		GetItem(char chPosType, std::int32_t lItemID);
	SItemGrid*		GetItem2(char chPosType, std::int32_t lPosID);
	bool			SetEquipValid(char chEquipPos, bool bValid, bool bSyn = true);
	bool			SetKitbagItemValid(int16_t sPosID, bool bValid, bool bRecheckAttr = true, bool bSyn = true);
	bool			SetKitbagItemValid(SItemGrid* pSItem, bool bValid, bool bRecheckAttr = true, bool bSyn = true);
	bool			ItemIsAppendLook(SItemGrid* pSItem);
	void			SetLookChangeFlag(bool bChange = false);
	void			SetEspeItemChangeFlag(bool bChange = false);
	char		GetLookChangeNum(void);
	bool			CheckForgeItem(SForgeItem *pSItem = NULL);
	bool			DoForgeLikeScript(const char *cszFunc, std::int32_t &lRet);
	bool			DoLifeSkillcript(const char *cszFunc, std::int32_t &lRet);
	bool			DoTigerScript(const char *cszFunc);
	void			SetInOutMapQueue(bool bOutMap = true);
	bool			InOutMapQueue(void);
	bool			AddKitbagCapacity(int16_t sAddVal);
	void			CheckItemValid(SItemGrid* pCItem);
	void			CheckEspeItemGrid(void);
	int16_t		KbPushItem(bool bRecheckAttr, bool bSynAttr, SItemGrid *pGrid, int16_t &sPosID, int16_t sType = 0, bool bCommit = true, bool bSureOpr = false);
	int16_t		KbPopItem(bool bRecheckAttr, bool bSynAttr, SItemGrid *pGrid, int16_t sPosID, int16_t sType = 0, bool bCommit = true);
	int16_t		KbClearItem(bool bRecheckAttr, bool bSynAttr, int16_t sPosID, int16_t sType = 0);
	int16_t		KbClearItem(bool bRecheckAttr, bool bSynAttr, SItemGrid *pGrid, int16_t sNum = 0);
	int16_t		KbRegroupItem(bool bRecheckAttr, bool bSynAttr, int16_t sSrcPosID, int16_t sSrcNum, int16_t sTarPosID, int16_t sType = 0);
	void			ResetScriptParam(void);
	std::int32_t		GetScriptParam(char chID);
	bool			SetScriptParam(char chID, std::int32_t lVal);
	void			CheckBagItemValid(CKitbag* pCBag);
	void			CheckLookItemValid(void);
	bool			String2LookDate(std::string &strData);
	bool			String2KitbagData(std::string &strData);
    bool            String2KitbagTmpData(std::string &strData);


	void	SetKitbagRecDBID(std::int32_t lDBID);
	std::int32_t	GetKitbagRecDBID(void);

    //ID
    void	SetKitbagTmpRecDBID(std::int32_t lDBID);
	std::int32_t	GetKitbagTmpRecDBID(void);

	void	LogAssets(char chLType);
	bool	SaveAssets(void);
	bool	IsRangePoint(std::int32_t lPosX, std::int32_t lPosY, std::int32_t lDist);
	bool	IsRangePoint(const Corsairs::Util::Point &SPos, std::int32_t lDist);
	bool	IsRangePoint2(std::int32_t lPosX, std::int32_t lPosY, std::int32_t lDist2);
	bool	IsRangePoint2(const Corsairs::Util::Point &SPos, std::int32_t lDist2);
	void	SetDBSaveInterval(std::int32_t lIntl);
	std::int32_t	GetDBSaveInterval(void);
	void	ResetPosState(void);

	int		GetLotteryIssue();

	DWORD				m_dwBoatCtrlTick; // 

	// AI----------------------------------------------------------------
	//  CAICharacter, Character
	BYTE				m_AIType;
	CCharacter*			m_AITarget;		
	CCharacter*			m_HostCha;		  // 
	int					m_nPatrolX;       // 
	int					m_nPatrolY;
	short				m_sChaseRange;    // , 
	BYTE				m_btPatrolState;  // , 0 1
	                                      //           1 2
                                          //           2 12
	                                      //           3 21
public:

	void	ResetAIState();				  // AI, 

	BOOL		GetChaRelive();
	void		SetChaRelive();
	void		ResetChaRelive();

	void		SetVolunteer(BOOL bVol);
	BOOL		IsVolunteer();
	void		SetInvited(BOOL bInvited);
	BOOL		IsInvited();

	void		SetCredit(std::int32_t lCredit);
	std::int32_t	GetCredit();
	void			AddMasterCredit(std::int32_t lCredit);
	std::uint32_t	GetMasterDBID();

	std::int32_t			GetStoreItemID();
	void			SetStoreItemID(std::int32_t lStoreItemID);
	bool			IsStoreBuy();
	void			SetStoreBuy(bool bValue);
	int				GetPetNum();
	void			SetPetNum(int nPetNum);

	bool			CheckStoreTime(DWORD dwInterval);
	void			ResetStoreTime();

	bool			IsStoreEnable();
	void			SetStoreEnable(bool bStoreEnable);

    //
    bool            IsScaleFlag();
    void            SetScaleFlag();
    void            SetExpScale(DWORD scale);
    DWORD           GetExpScale();
    int m_noticeState;//
	int m_retry3;
	int m_retry4;
	int m_retry5;
    int m_retry6;

	std::int64_t guildPermission{};

	unsigned int chatColour;
protected:

public:

    virtual void OnBeginSee(Entity *);
	virtual void OnEndSee(Entity *);
    virtual void OnBeginSeen(CCharacter *pCCha);
	virtual void OnEndSeen(CCharacter *pCCha);
	virtual void AreaChange(void);

	virtual void OnAI(DWORD dwCurTime);
    virtual void OnAreaCheck(DWORD dwCurTime);
	virtual void OnTeamNotice(DWORD dwCurTime);
    virtual void OnDBUpdate(DWORD dwCurTime);

	virtual void BeginAction(const Corsairs::Net::Msg::CmBeginActionMessage& msg);
	virtual void AfterStepMove(void);
	virtual void SubsequenceFight();
	virtual void SubsequenceMove();

	void	OnDie(DWORD dwCurTime);
	void	SrcFightTar(CFightAble *pTar, int16_t sSkillID);
	
	void	DoCommand(const char *cszCommand, std::uint32_t ulLen);
	BOOL	DoGMCommand(const char *pszCmd, const char *pszParam);
	void	DoCommand_CheckStatus(const char *pszCommand, std::uint32_t ulLen);
	void	HandleHelp(const char *pszCommand, std::uint32_t ulLen);

	std::int32_t	ExecuteEvent(Entity *pCObj, std::uint16_t usEventID);

	void	SetActControl(Corsairs::Common::Character::ActControl ctrlType, bool set = true);

	bool		CanLearnSkill(CSkillRecord *pCSkill, char chToLv);
	int16_t	CanEquipItem(int16_t sItemID);
	int16_t	CanEquipItemNew(int16_t sItemID1, int16_t sItemID2 = 0);

	int16_t	CanEquipItem(SItemGrid* pSEquipIt);

	int16_t	IsItemExpired(SItemGrid* pSEquipIt);

public:
	CKitbag				m_CKitbag;			// 
	CKitbag				*m_pCKitbagTmp;       // 
	stNetShortCut		m_CShortcut;		// 
	std::int32_t				m_lKbRecDBID;		// ID
	std::int32_t				m_lKbTmpRecDBID;	// ID
	std::int32_t				m_lStoreItemID;
	bool				m_bStoreBuy;
	DWORD				m_dwStoreTime;
	bool				m_bStoreEnable;
	//cooldown for ranking
	DWORD GuildBankCD{ 0 };
	DWORD				ShowRankColD;	
	int					m_nPetNum;
	
	stNetChangeChaPart	m_SChaPart;
	std::array<bool, Corsairs::Common::Character::kActControlCount> _actControl{}; //
	CTimer				m_timerScripts;						// HPSP

	CHateMgr*			m_pHate;							// 

	BOOL				m_bRelive;							// 

	BOOL				m_bVol;								// 
	BOOL				m_bInvited;							// 

	//SSkillGrid			*m_pSkillGridTemp;

	// 
	struct
	{
		char	m_chSelRelive;	// 
		char	m_chReliveLv;	// 0.
	};

	CActionCache		m_CActCache;

	DWORD	m_dwCellRunTime[16];

	short	m_sTigerItemID[9];	// 9ID
	short	m_sTigerSel[3];		// 
	
private:
	BOOL BoatEnterMap( CCharacter& Boat, DWORD dwxPos, DWORD dwyPos, USHORT sDir );

	char			m_szMotto[defMOTTO_LEN];
	std::uint16_t			m_usIcon;

	// Режим разработчика. Исходные характеристики запоминаются при включении,
	// чтобы `dev off` вернул персонажа как был, а не к жёстко заданным числам.
	bool			_devInvincible{false};
	bool			_devSaved{false};
	std::int32_t	_devOrigSpeed{0};
	std::int32_t	_devOrigMinAtk{0};
	std::int32_t	_devOrigMaxAtk{0};
	std::int32_t	_devOrigDef{0};

    bool m_expFlag;
    DWORD m_ExpScale;       //  

	struct
	{
		short m_sPoseState; // 0.1
	};
	//add by jilinlee 2007/4/20
	struct SReadBook
	{
		bool bIsReadState;       //0,.1.
        DWORD dwLastReadCallTick; //Reading_Book.
	};
    
	SReadBook m_SReadBook;

	CAction		        m_CAction;
	SLean		        m_SLean;
	SSeat				m_SSeat;

	SCheatX				m_sCheatX;
	
	BYTE				_btBlockCnt;
	STempChaPart        m_STempChaPart;

	int chaIMP;
	CCharacter* mountCha;
	BOOL		IsChaOnMount;
	

	// 
	Corsairs::Common::Mission::CTradeData*	m_pTradeData;

	#define CHAEXIT_NONE				0		// ...
	#define CHAEXIT_BEGIN				1<<0	// 	

	BYTE				m_byExit;
	CTimer				m_timerExit;
    CTimer              m_timerAI;
    CTimer              m_timerAreaCheck;
	CTimer				m_timerDBUpdate;
	CTimer				m_timerDie;
	CTimer				m_timerMission;			// 
    CTimer				m_timerSkillState;		// 
	CTimer              m_timerTeam;			// Team
	struct
	{
		CTimer			m_timerPing;
		std::uint32_t		m_ulPingDataLen;
	};

	std::bitset<8>		m_chPKCtrl;

	std::int32_t			m_lSideID;				//
	bool				m_bInOutMapQueue;
	struct // Ping
	{
		DWORD	m_dwPing;
		DWORD	m_dwPingRec[defPING_RECORD_NUM];
		DWORD	m_dwPingSendTick;
	};

	struct // for test net state
	{
		std::uint32_t	m_ulNetSendLen;
		CTimer		m_timerNetSendFreq;
	};

	std::int32_t	m_lScriptParam[defCHA_SCRIPT_PARAM_NUM];

protected:

    DWORD   _dwLastAreaTick;
    BYTE    _btLastAreaNo;

	DWORD	_dwLastSayTick;
	

public:
	void    ResetLifeTime(DWORD dwTime);
	BOOL    CheckLifeTime();
	DWORD   GetLifeTime();

	DWORD	_dwLifeTime;
	DWORD	_dwLifeTimeTick;

	bool    IsOfflineMode() const;
	DWORD	_dwStallTick;

	bool	appCheck[Corsairs::Common::Network::enumEQUIP_NUM];

	BYTE	requestType;
	Corsairs::Util::Square	requestPos; // must check if player is in same position

	bool    IsReqPosEqualRealPos();
};

// 
extern Corsairs::Util::Point		g_SSkillPoint;
extern bool			g_bBeatBack;
extern unsigned char	g_uchFightID;
extern char			g_chUseItemFailed[2];
extern char			g_chUseItemGiveMission[2];
//
extern bool IsPersistStateID(unsigned char uchStateID);
extern char* SStateData2String(CCharacter *pCCha, char *szSStateBuf, int nLen, char chSaveType);

/**
 * [p]ersist
 * Saves any player stats that were configured in GameServer[x].cfg
 * 07.27.2018
 */
extern bool		Strin2SStateData(CCharacter *pCCha, std::string &strData);
//Add by lark.li 20080723
extern char*	ChaExtendAttr2String(CCharacter *pCCha, char *szAttrBuf, int nLen);
extern bool		Strin2ChaExtendAttr(CCharacter *pCCha, std::string &strAttr);
 //
extern void TL(int nType, const char *pszCha1, const char *pszCha2, const char *pszTrade);

#endif // CHARACTER_H
