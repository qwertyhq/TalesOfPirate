#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsSession.h"

#include "CorsairsNet/include/CommandMessages.h"
#include "CorsairsNet/include/Packet.h"
#include "Misc/AutomationTest.h"

namespace
{
using Corsairs::Net::RPacket;
using Corsairs::Net::WPacket;
namespace Msg = Corsairs::Net::Msg;

constexpr int64 LocalWorldId = 77;
constexpr int64 OtherWorldId = 88;

template <typename MessageType>
void Deliver(UCorsairsSession* Session, const MessageType& Message)
{
	WPacket Wire = Msg::serialize(Message);
	RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}

Msg::SkillEntry MakeWireSkill(
	const int64 SkillId,
	const int64 State,
	const int64 Level,
	const int64 Seed)
{
	Msg::SkillEntry Skill;
	Skill.id = SkillId;
	Skill.state = State;
	Skill.level = Level;
	Skill.useSp = Seed + 1;
	Skill.useEndure = Seed + 2;
	Skill.useEnergy = Seed + 3;
	Skill.resumeTime = Seed + 4;
	Skill.range[0] = Seed + 5;
	Skill.range[1] = Seed + 6;
	Skill.range[2] = Seed + 7;
	Skill.range[3] = Seed + 8;
	return Skill;
}

Msg::McEnterMapMessage MakeEnterMap()
{
	Msg::McEnterMapMessage Message;
	Message.errCode = 0;
	Message.data.emplace();
	auto& Data = Message.data.value();
	Data.mapName = "garner";
	Data.baseInfo.worldId = LocalWorldId;
	Data.baseInfo.name = "Tester";
	Data.baseInfo.posX = 1000;
	Data.baseInfo.posY = 2000;
	Data.baseInfo.handle = 7007;
	Data.baseInfo.look.typeId = 1;
	Data.baseInfo.guildId = 501;
	Data.baseInfo.teamLeaderId = 502;
	Data.baseInfo.side.sideId = 503;
	Data.baseInfo.pkCtrl = 504;
	Data.skillBag.defSkillId = 26;
	Data.skillBag.synType = 0;
	Data.skillBag.skills = {
		MakeWireSkill(26, 2, 3, 100),
		MakeWireSkill(42, 4, 5, 200),
	};
	for (int32 Slot = 0; Slot < Msg::SHORT_CUT_NUM; ++Slot)
	{
		Data.shortcut.entries[Slot].type = 10 + Slot;
		Data.shortcut.entries[Slot].gridId = 1000 + Slot;
	}
	return Message;
}

const FCorsairsSkillEntry* FindSkill(
	const UCorsairsSession* Session,
	const int64 SkillId)
{
	return Session->GetSkillBag().FindByPredicate(
		[SkillId](const FCorsairsSkillEntry& Entry)
		{
			return Entry.SkillId == SkillId;
		});
}

bool HasFullSkillPayload(const FCorsairsSkillEntry* Skill)
{
	return Skill != nullptr &&
		Skill->SkillId == 26 &&
		Skill->State == 2 &&
		Skill->Level == 3 &&
		Skill->UseSp == 101 &&
		Skill->UseEndure == 102 &&
		Skill->UseEnergy == 103 &&
		Skill->ResumeTime == 104 &&
		Skill->Range.Num() == 4 &&
		Skill->Range[0] == 105 &&
		Skill->Range[1] == 106 &&
		Skill->Range[2] == 107 &&
		Skill->Range[3] == 108;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionEnterMapSkillStateTest,
	"Corsairs.Net.Session.SkillState.EnterMapCommitsCompleteSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionEnterMapSkillStateTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	int32 Notifications = 0;
	bool bObservedCompleteState = false;
	Session->SetSkillStateObserverForTests(
		[&]()
		{
			++Notifications;
			bObservedCompleteState =
				Session->GetDefaultSkillId() == 26 &&
				Session->GetSkillBag().Num() == 2 &&
				HasFullSkillPayload(FindSkill(Session, 26)) &&
				Session->GetShortcuts().Num() == Msg::SHORT_CUT_NUM &&
				Session->GetShortcuts()[35].Slot == 35 &&
				Session->GetShortcuts()[35].Type == 45 &&
				Session->GetShortcuts()[35].GridId == 1035 &&
				Session->GetLocalActor().TargetPolicy.GuildId == 501;
		});

	Deliver(Session, MakeEnterMap());

	TestEqual(TEXT("один снимок состояния"), Notifications, 1);
	TestTrue(TEXT("делегат видит атомарно собранный снимок"), bObservedCompleteState);
	TestTrue(TEXT("полный wire payload навыка сохранён"),
		HasFullSkillPayload(FindSkill(Session, 26)));
	TestEqual(TEXT("панель содержит все 36 ячеек"),
		Session->GetShortcuts().Num(), Msg::SHORT_CUT_NUM);
	TestEqual(TEXT("граница первого ряда"), Session->GetShortcuts()[11].Slot, 11);
	TestEqual(TEXT("начало второго ряда"), Session->GetShortcuts()[12].Slot, 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionSkillBagDeltaTest,
	"Corsairs.Net.Session.SkillState.InitAddModifyDeleteAndRejectUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionSkillBagDeltaTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, FIntPoint(1000, 2000));
	int32 Notifications = 0;
	int32 ProtocolErrors = 0;
	Session->SetSkillStateObserverForTests([&]() { ++Notifications; });
	Session->SetEventObserversForTests(
		[](const FCorsairsMovementEvent&) {},
		[&](const FString&) { ++ProtocolErrors; },
		[](ECorsairsLoginStage) {});

	Msg::McSynSkillBagMessage Init;
	Init.worldId = LocalWorldId;
	Init.skillBag.defSkillId = 26;
	Init.skillBag.synType = 0;
	Init.skillBag.skills = {
		MakeWireSkill(26, 2, 3, 100),
		MakeWireSkill(42, 4, 5, 200),
	};
	Deliver(Session, Init);
	TestEqual(TEXT("INIT заменяет снимок"), Session->GetSkillBag().Num(), 2);
	TestTrue(TEXT("INIT хранит все поля"), HasFullSkillPayload(FindSkill(Session, 26)));

	Msg::McSynSkillBagMessage Add;
	Add.worldId = LocalWorldId;
	Add.skillBag.defSkillId = 30;
	Add.skillBag.synType = 1;
	Add.skillBag.skills = {
		MakeWireSkill(26, 7, 8, 300),
		MakeWireSkill(30, 9, 10, 400),
	};
	Deliver(Session, Add);
	TestEqual(TEXT("ADD делает upsert"), Session->GetSkillBag().Num(), 3);
	TestEqual(TEXT("ADD обновляет существующий уровень"), FindSkill(Session, 26)->Level, 8);
	TestEqual(TEXT("ADD обновляет default"), Session->GetDefaultSkillId(), int64{30});

	Msg::McSynSkillBagMessage Modify;
	Modify.worldId = LocalWorldId;
	Modify.skillBag.defSkillId = 42;
	Modify.skillBag.synType = 2;
	Modify.skillBag.skills = {
		MakeWireSkill(26, 0, 0, 500),
		MakeWireSkill(42, 11, 12, 600),
	};
	Deliver(Session, Modify);
	TestEqual(TEXT("MODI удаляет нулевой уровень"), Session->GetSkillBag().Num(), 2);
	TestNull(TEXT("удалённого навыка нет"), FindSkill(Session, 26));
	TestEqual(TEXT("MODI делает upsert"), FindSkill(Session, 42)->State, 11);

	const TArray<FCorsairsSkillEntry> BeforeUnknown = Session->GetSkillBag();
	const int64 DefaultBeforeUnknown = Session->GetDefaultSkillId();
	Msg::McSynSkillBagMessage Unknown;
	Unknown.worldId = LocalWorldId;
	Unknown.skillBag.defSkillId = 999;
	Unknown.skillBag.synType = 99;
	Unknown.skillBag.skills = {MakeWireSkill(999, 1, 1, 700)};
	Deliver(Session, Unknown);
	TestEqual(TEXT("неизвестный тип не меняет размер"),
		Session->GetSkillBag().Num(), BeforeUnknown.Num());
	TestEqual(TEXT("неизвестный тип сохраняет первый id"),
		Session->GetSkillBag()[0].SkillId, BeforeUnknown[0].SkillId);
	TestEqual(TEXT("неизвестный тип сохраняет default"),
		Session->GetDefaultSkillId(), DefaultBeforeUnknown);
	TestEqual(TEXT("неизвестный тип сообщает ошибку"), ProtocolErrors, 1);
	TestEqual(TEXT("только три валидных публикации"), Notifications, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionDefaultAndShortcutTest,
	"Corsairs.Net.Session.SkillState.DefaultAndLiveShortcutRequireLocalActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionDefaultAndShortcutTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, FIntPoint(1000, 2000));
	int32 Notifications = 0;
	Session->SetSkillStateObserverForTests([&]() { ++Notifications; });

	Deliver(Session, Msg::McSynDefaultSkillMessage{OtherWorldId, 900});
	TestEqual(TEXT("чужой default проигнорирован"), Session->GetDefaultSkillId(), int64{0});
	Deliver(Session, Msg::McSynDefaultSkillMessage{LocalWorldId, 26});
	TestEqual(TEXT("локальный default принят"), Session->GetDefaultSkillId(), int64{26});

	Msg::ChaShortcutInfo Shortcut;
	for (int32 Slot = 0; Slot < Msg::SHORT_CUT_NUM; ++Slot)
	{
		Shortcut.entries[Slot].type = 100 + Slot;
		Shortcut.entries[Slot].gridId = 2000 + Slot;
	}
	Msg::McCharacterActionMessage Action;
	Action.worldId = OtherWorldId;
	Action.packetId = 1;
	Action.actionType = Msg::ActionType::SHORTCUT;
	Action.data = Shortcut;
	Deliver(Session, Action);
	TestEqual(TEXT("чужие shortcuts проигнорированы"), Session->GetShortcuts().Num(), 0);

	Action.worldId = LocalWorldId;
	Action.packetId = 2;
	Deliver(Session, Action);
	TestEqual(TEXT("live snapshot содержит 36 ячеек"),
		Session->GetShortcuts().Num(), Msg::SHORT_CUT_NUM);
	TestEqual(TEXT("slot сохраняет wire index"), Session->GetShortcuts()[35].Slot, 35);
	TestEqual(TEXT("slot сохраняет type"), Session->GetShortcuts()[35].Type, 135);
	TestEqual(TEXT("slot сохраняет grid"), Session->GetShortcuts()[35].GridId, int64{2035});
	TestEqual(TEXT("default и shortcut опубликованы отдельно"), Notifications, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionSkillStateResetTest,
	"Corsairs.Net.Session.SkillState.DisconnectAndLogoutClearState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionSkillStateResetTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Disconnected = NewObject<UCorsairsSession>();
	Deliver(Disconnected, MakeEnterMap());
	Disconnected->HandleConnectionStateForTests(
		ECorsairsConnectionState::Disconnected, TEXT("test"));
	TestEqual(TEXT("disconnect очищает skills"), Disconnected->GetSkillBag().Num(), 0);
	TestEqual(TEXT("disconnect очищает shortcuts"), Disconnected->GetShortcuts().Num(), 0);
	TestEqual(TEXT("disconnect очищает default"), Disconnected->GetDefaultSkillId(), int64{0});

	UCorsairsSession* LoggedOut = NewObject<UCorsairsSession>();
	Deliver(LoggedOut, MakeEnterMap());
	LoggedOut->Logout();
	TestEqual(TEXT("logout очищает skills"), LoggedOut->GetSkillBag().Num(), 0);
	TestEqual(TEXT("logout очищает shortcuts"), LoggedOut->GetShortcuts().Num(), 0);
	TestEqual(TEXT("logout очищает default"), LoggedOut->GetDefaultSkillId(), int64{0});
	return true;
}

#endif
