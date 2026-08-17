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
constexpr int64 EntitySkillId = 0xFFFFFFFFLL;
constexpr int64 GroundSkillId = 4;
constexpr int64 TargetWorldId = 0xFFFFFFFFLL;
constexpr int64 TargetHandle = MIN_int32;
const FIntPoint Spawn(1000, 2000);
const TArray<FIntPoint> ApproachPath{
	Spawn,
	FIntPoint(1300, 2400),
};

template <typename MessageType>
void Deliver(UCorsairsSession* Session, const MessageType& Message)
{
	WPacket Wire = Msg::serialize(Message);
	RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}

void InstallSkill(
	UCorsairsSession* Session,
	const int64 SkillId,
	const int64 Level)
{
	Msg::McSynSkillBagMessage Message;
	Message.worldId = LocalWorldId;
	Message.skillBag.synType = 0;
	Msg::SkillEntry Skill;
	Skill.id = SkillId;
	Skill.level = Level;
	Message.skillBag.skills.push_back(Skill);
	Deliver(Session, Message);
}

FCorsairsWorldActor MakeTarget(
	const int64 WorldId = TargetWorldId,
	const int64 Handle = TargetHandle)
{
	FCorsairsWorldActor Target;
	Target.WorldId = WorldId;
	Target.Handle = Handle;
	Target.Position = ApproachPath.Last();
	return Target;
}

void TestResult(
	FAutomationTestBase* Test,
	const TCHAR* What,
	const ECorsairsActionRequestResult Actual,
	const ECorsairsActionRequestResult Expected)
{
	Test->TestEqual(
		What,
		static_cast<uint8>(Actual),
		static_cast<uint8>(Expected));
}

bool HasExactPath(
	const std::string& Bytes,
	const TArray<FIntPoint>& Expected)
{
	return Bytes.size() ==
			static_cast<std::size_t>(Expected.Num() * sizeof(FIntPoint)) &&
		FMemory::Memcmp(
			Bytes.data(),
			Expected.GetData(),
			Bytes.size()) == 0;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionEntitySkillWireTest,
	"Corsairs.Net.Session.Skill.EntityExactWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionEntitySkillWireTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	InstallSkill(Session, EntitySkillId, 1);
	Session->AddVisibleActorForTests(MakeTarget());
	bool bWireMatches = false;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			Msg::CmBeginActionMessage Message;
			Msg::deserialize(Packet, Message);
			const Msg::CmActionSkillInputData* Skill =
				std::get_if<Msg::CmActionSkillInputData>(&Message.data);
			bWireMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Message.worldId == LocalWorldId &&
				Message.packetId == 1 &&
				Message.actionType == Msg::ActionType::SKILL &&
				Skill != nullptr &&
				Skill->chMove == 2 &&
				Skill->fightId == 1 &&
				Skill->skillId == EntitySkillId &&
				Skill->tarInfo1 == TargetWorldId &&
				Skill->tarInfo2 == TargetHandle &&
				HasExactPath(Skill->pathData, ApproachPath);
			return true;
		});

	TestResult(this, TEXT("entity skill отправлен"),
		Session->UseSkillOn(
			EntitySkillId,
			TargetWorldId,
			TargetHandle,
			ApproachPath),
		ECorsairsActionRequestResult::Sent);
	TestTrue(TEXT("entity wire сохраняет uint32 id, int32 handle и путь"),
		bWireMatches);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionGroundSkillWireTest,
	"Corsairs.Net.Session.Skill.GroundExactWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionGroundSkillWireTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	InstallSkill(Session, GroundSkillId, 1);
	const FIntPoint ExactGround(MIN_int32 + 1, MAX_int32);
	bool bWireMatches = false;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			Msg::CmBeginActionMessage Message;
			Msg::deserialize(Packet, Message);
			const Msg::CmActionSkillInputData* Skill =
				std::get_if<Msg::CmActionSkillInputData>(&Message.data);
			bWireMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Message.worldId == LocalWorldId &&
				Message.packetId == 1 &&
				Message.actionType == Msg::ActionType::SKILL &&
				Skill != nullptr &&
				Skill->chMove == 2 &&
				Skill->fightId == 1 &&
				Skill->skillId == GroundSkillId &&
				Skill->tarInfo1 == ExactGround.X &&
				Skill->tarInfo2 == ExactGround.Y &&
				HasExactPath(Skill->pathData, ApproachPath);
			return true;
		});

	TestResult(this, TEXT("ground skill отправлен"),
		Session->UseSkillAtPoint(
			GroundSkillId,
			ExactGround,
			ApproachPath),
		ECorsairsActionRequestResult::Sent);
	TestTrue(TEXT("ground wire сохраняет exact point отдельно от пути"),
		bWireMatches);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionSkillValidationTest,
	"Corsairs.Net.Session.Skill.ValidationAndRevalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionSkillValidationTest::RunTest(const FString&)
{
	const auto MakeSession = []()
	{
		UCorsairsSession* Session = NewObject<UCorsairsSession>();
		Session->SetInWorldForTests(LocalWorldId, Spawn);
		Session->AddVisibleActorForTests(MakeTarget());
		return Session;
	};

	UCorsairsSession* Absent = MakeSession();
	int32 AbsentSends = 0;
	Absent->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++AbsentSends;
			return true;
		});
	TestResult(this, TEXT("отсутствующий в bag skill отклонён"),
		Absent->UseSkillOn(
			EntitySkillId, TargetWorldId, TargetHandle, ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	InstallSkill(Absent, EntitySkillId, 0);
	TestResult(this, TEXT("нулевой Level отклонён"),
		Absent->UseSkillOn(
			EntitySkillId, TargetWorldId, TargetHandle, ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("непригодный bag не достигает transport"), AbsentSends, 0);

	UCorsairsSession* InvalidIdentity = MakeSession();
	InstallSkill(InvalidIdentity, EntitySkillId, 1);
	int32 InvalidIdentitySends = 0;
	InvalidIdentity->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++InvalidIdentitySends;
			return true;
		});
	TestResult(this, TEXT("stale handle отклонён"),
		InvalidIdentity->UseSkillOn(
			EntitySkillId, TargetWorldId, TargetHandle + 1, ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	TestResult(this, TEXT("WorldId вне uint32 отклонён"),
		InvalidIdentity->UseSkillOn(
			EntitySkillId,
			MAX_uint32 + 1LL,
			TargetHandle,
			ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	TestResult(this, TEXT("Handle вне int32 отклонён"),
		InvalidIdentity->UseSkillOn(
			EntitySkillId,
			TargetWorldId,
			MAX_int32 + 1LL,
			ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("невалидная identity не достигает transport"),
		InvalidIdentitySends, 0);

	UCorsairsSession* InvalidPath = MakeSession();
	InstallSkill(InvalidPath, EntitySkillId, 1);
	int32 InvalidPathSends = 0;
	InvalidPath->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++InvalidPathSends;
			return true;
		});
	TestResult(this, TEXT("skill path из одной точки отклонён"),
		InvalidPath->UseSkillAtPoint(
			EntitySkillId, FIntPoint(10, 20), {Spawn}),
		ECorsairsActionRequestResult::Invalid);
	TArray<FIntPoint> TooLong;
	TooLong.SetNum(33);
	TestResult(this, TEXT("skill path из 33 точек отклонён"),
		InvalidPath->UseSkillAtPoint(
			EntitySkillId, FIntPoint(10, 20), TooLong),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("невалидный skill path не достигает transport"),
		InvalidPathSends, 0);

	UCorsairsSession* StaleIdentity = MakeSession();
	InstallSkill(StaleIdentity, EntitySkillId, 1);
	int32 StaleIdentitySends = 0;
	StaleIdentity->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++StaleIdentitySends;
			return true;
		});
	StaleIdentity->SetMovementAuthorityObserverForTests(
		[&](const bool bLocked, const int64)
		{
			if (bLocked)
			{
				Deliver(
					StaleIdentity,
					Msg::McChaEndSeeMessage{1, TargetWorldId});
			}
		});
	TestResult(this, TEXT("исчезнувшая до send callback identity отклонена"),
		StaleIdentity->UseSkillOn(
			EntitySkillId, TargetWorldId, TargetHandle, ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("stale identity не достигает transport"),
		StaleIdentitySends, 0);

	UCorsairsSession* StalePath = MakeSession();
	InstallSkill(StalePath, EntitySkillId, 1);
	int32 StalePathSends = 0;
	StalePath->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++StalePathSends;
			return true;
		});
	TArray<FIntPoint> MutablePath = ApproachPath;
	StalePath->SetMovementAuthorityObserverForTests(
		[&](const bool bLocked, const int64)
		{
			if (bLocked)
			{
				MutablePath.Reset();
			}
		});
	TestResult(this, TEXT("изменённый до send callback путь отклонён"),
		StalePath->UseSkillOn(
			EntitySkillId, TargetWorldId, TargetHandle, MutablePath),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("stale path не достигает transport"), StalePathSends, 0);

	UCorsairsSession* StaleBag = MakeSession();
	InstallSkill(StaleBag, EntitySkillId, 1);
	int32 StaleBagSends = 0;
	StaleBag->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++StaleBagSends;
			return true;
		});
	StaleBag->SetMovementAuthorityObserverForTests(
		[&](const bool bLocked, const int64)
		{
			if (!bLocked)
			{
				return;
			}
			Msg::McSynSkillBagMessage Remove;
			Remove.worldId = LocalWorldId;
			Remove.skillBag.synType = 2;
			Msg::SkillEntry RemovedSkill;
			RemovedSkill.id = EntitySkillId;
			RemovedSkill.level = 0;
			Remove.skillBag.skills.push_back(RemovedSkill);
			Deliver(StaleBag, Remove);
		});
	TestResult(this, TEXT("удалённый до send callback skill отклонён"),
		StaleBag->UseSkillOn(
			EntitySkillId, TargetWorldId, TargetHandle, ApproachPath),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("stale bag не достигает transport"), StaleBagSends, 0);
	return true;
}

#endif
