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
constexpr int64 RemoteWorldId = 88;
constexpr int64 MissingWorldId = 99;

template <typename MessageType>
void DeliverActorState(UCorsairsSession* Session, const MessageType& Message)
{
	WPacket Wire = Msg::serialize(Message);
	RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}

Msg::McEnterMapMessage MakePolicyEnterMap()
{
	Msg::McEnterMapMessage Message;
	Message.errCode = 0;
	Message.data.emplace();
	auto& Data = Message.data.value();
	Data.mapName = "garner";
	Data.baseInfo.worldId = LocalWorldId;
	Data.baseInfo.name = "Local";
	Data.baseInfo.handle = 7007;
	Data.baseInfo.guildId = 101;
	Data.baseInfo.teamLeaderId = 102;
	Data.baseInfo.side.sideId = 103;
	Data.baseInfo.pkCtrl = 104;
	return Message;
}

Msg::McChaBeginSeeMessage MakePolicyActorSeen()
{
	Msg::McChaBeginSeeMessage Message;
	Message.seeType = 1;
	Message.base.worldId = RemoteWorldId;
	Message.base.name = "Remote";
	Message.base.handle = 8008;
	Message.base.guildId = 201;
	Message.base.teamLeaderId = 202;
	Message.base.side.sideId = 203;
	Message.base.pkCtrl = 204;
	return Message;
}

const FCorsairsWorldActor* FindVisible(
	const UCorsairsSession* Session,
	const int64 WorldId)
{
	return Session->GetVisibleActors().FindByPredicate(
		[WorldId](const FCorsairsWorldActor& Actor)
		{
			return Actor.WorldId == WorldId;
		});
}

bool IsPolicy(
	const FCorsairsTargetPolicy& Policy,
	const int64 GuildId,
	const int64 TeamLeaderId,
	const int32 SideId,
	const int32 PkControl)
{
	return Policy.GuildId == GuildId &&
		Policy.TeamLeaderId == TeamLeaderId &&
		Policy.SideId == SideId &&
		Policy.PkControl == PkControl;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionTargetPolicySnapshotTest,
	"Corsairs.Net.Session.TargetPolicy.EnterMapAndBeginSeePreserveWirePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionTargetPolicySnapshotTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	int32 Notifications = 0;
	bool bObservedAtomicEnterMap = false;
	Session->SetTargetPolicyObserverForTests(
		[&](const FCorsairsWorldActor& Actor)
		{
			++Notifications;
			if (Actor.WorldId == LocalWorldId)
			{
				bObservedAtomicEnterMap =
					IsPolicy(Actor.TargetPolicy, 101, 102, 103, 104) &&
					Session->GetShortcuts().Num() == Msg::SHORT_CUT_NUM;
			}
		});

	DeliverActorState(Session, MakePolicyEnterMap());
	TestTrue(TEXT("локальная policy взята из ChaBaseInfo"),
		IsPolicy(Session->GetLocalActor().TargetPolicy, 101, 102, 103, 104));
	TestTrue(TEXT("делегат входа видит полный commit"), bObservedAtomicEnterMap);

	DeliverActorState(Session, MakePolicyActorSeen());
	const FCorsairsWorldActor* Remote = FindVisible(Session, RemoteWorldId);
	TestNotNull(TEXT("удалённый актёр добавлен"), Remote);
	TestTrue(TEXT("удалённая policy взята из ChaBaseInfo"),
		Remote != nullptr && IsPolicy(Remote->TargetPolicy, 201, 202, 203, 204));
	TestEqual(TEXT("policy опубликована для local и remote"), Notifications, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionTargetPolicyLiveUpdateTest,
	"Corsairs.Net.Session.TargetPolicy.LiveCommandsUpdateExactActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionTargetPolicyLiveUpdateTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, FIntPoint::ZeroValue);
	FCorsairsWorldActor Remote;
	Remote.WorldId = RemoteWorldId;
	Remote.TargetPolicy = FCorsairsTargetPolicy{201, 202, 203, 204};
	Session->AddVisibleActorForTests(Remote);

	int32 Notifications = 0;
	TArray<int64> NotifiedWorldIds;
	Session->SetTargetPolicyObserverForTests(
		[&](const FCorsairsWorldActor& Actor)
		{
			++Notifications;
			NotifiedWorldIds.Add(Actor.WorldId);
		});

	DeliverActorState(Session, Msg::McSynTLeaderIdMessage{LocalWorldId, 1002});
	Msg::McSynSideInfoMessage Side;
	Side.worldId = RemoteWorldId;
	Side.side.sideId = 2003;
	DeliverActorState(Session, Side);
	DeliverActorState(Session, Msg::McGuildInfoMessage{
		RemoteWorldId, 2001, "Guild", "Motto", 7});

	Msg::McCharacterActionMessage PkAction;
	PkAction.worldId = LocalWorldId;
	PkAction.packetId = 1;
	PkAction.actionType = Msg::ActionType::PK_CTRL;
	PkAction.data = Msg::ActionPkCtrlData{1004};
	DeliverActorState(Session, PkAction);

	TestTrue(TEXT("local меняет только leader и pk"),
		IsPolicy(Session->GetLocalActor().TargetPolicy, 0, 1002, 0, 1004));
	const FCorsairsWorldActor* UpdatedRemote = FindVisible(Session, RemoteWorldId);
	TestTrue(TEXT("remote меняет только side и guild"),
		UpdatedRemote != nullptr &&
		IsPolicy(UpdatedRemote->TargetPolicy, 2001, 202, 2003, 204));
	TestEqual(TEXT("по одному событию на точное обновление"), Notifications, 4);

	DeliverActorState(Session, Msg::McSynTLeaderIdMessage{MissingWorldId, 9999});
	Msg::McSynSideInfoMessage MissingSide;
	MissingSide.worldId = MissingWorldId;
	MissingSide.side.sideId = 9999;
	DeliverActorState(Session, MissingSide);
	DeliverActorState(Session, Msg::McGuildInfoMessage{
		MissingWorldId, 9999, "Missing", "Missing", 0});
	PkAction.worldId = MissingWorldId;
	PkAction.packetId = 2;
	PkAction.data = Msg::ActionPkCtrlData{9999};
	DeliverActorState(Session, PkAction);
	TestEqual(TEXT("неизвестный actor не публикуется"), Notifications, 4);
	TestTrue(TEXT("неизвестные команды не задели local"),
		IsPolicy(Session->GetLocalActor().TargetPolicy, 0, 1002, 0, 1004));
	UpdatedRemote = FindVisible(Session, RemoteWorldId);
	TestTrue(TEXT("неизвестные команды не задели remote"),
		UpdatedRemote != nullptr &&
		IsPolicy(UpdatedRemote->TargetPolicy, 2001, 202, 2003, 204));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionTargetPolicyResetTest,
	"Corsairs.Net.Session.TargetPolicy.DisconnectAndLogoutClearPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionTargetPolicyResetTest::RunTest(const FString& Parameters)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	DeliverActorState(Session, MakePolicyEnterMap());
	DeliverActorState(Session, MakePolicyActorSeen());
	Session->HandleConnectionStateForTests(
		ECorsairsConnectionState::Disconnected, TEXT("test"));
	TestTrue(TEXT("disconnect очищает local policy"),
		IsPolicy(Session->GetLocalActor().TargetPolicy, 0, 0, 0, 0));
	const FCorsairsWorldActor* Remote = FindVisible(Session, RemoteWorldId);
	TestTrue(TEXT("disconnect очищает visible policy"),
		Remote != nullptr && IsPolicy(Remote->TargetPolicy, 0, 0, 0, 0));

	UCorsairsSession* LoggedOut = NewObject<UCorsairsSession>();
	DeliverActorState(LoggedOut, MakePolicyEnterMap());
	LoggedOut->Logout();
	TestTrue(TEXT("logout очищает local policy"),
		IsPolicy(LoggedOut->GetLocalActor().TargetPolicy, 0, 0, 0, 0));
	return true;
}

#endif
