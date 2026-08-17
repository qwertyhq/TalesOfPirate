#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacter.h"
#include "CorsairsGameMode.h"
#include "CorsairsPlayerController.h"
#include "CorsairsPlayerCharacter.h"

#include "Components/CapsuleComponent.h"
#include "CorsairsNet/include/CommandMessages.h"
#include "CorsairsNet/include/Packet.h"
#include "CorsairsSession.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

namespace
{
	using Corsairs::Net::WPacket;
	using Corsairs::Net::RPacket;

	constexpr int64 LocalWorldId = 77;
	constexpr double ServerSpeedCmPerSecond = 200.0;

	FActorSpawnParameters MovementRoutingAlwaysSpawnParameters()
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Parameters;
	}

	int32 CountMovementBindings(
		const UCorsairsSession* Session,
		const UObject* Object)
	{
		int32 Count = 0;
		for (const UObject* BoundObject :
			Session->OnMovementChanged.GetAllObjects())
		{
			if (BoundObject == Object)
			{
				++Count;
			}
		}
		return Count;
	}

	FCorsairsMovementEvent MakeAcceptedPath(
		const int64 WorldId,
		const bool bLocal,
		const bool bServerDriven,
		const double Speed,
		const TArray<FIntPoint>& Waypoints)
	{
		FCorsairsMovementEvent Event;
		Event.WorldId = WorldId;
		Event.Type = ECorsairsMovementEventType::AcceptedPath;
		Event.Waypoints = Waypoints;
		Event.Endpoint = Waypoints.IsEmpty()
			? FIntPoint::ZeroValue
			: Waypoints.Last();
		Event.bLocal = bLocal;
		Event.bServerDriven = bServerDriven;
		Event.MovementSpeedCmPerSecond = Speed;
		return Event;
	}

	FCorsairsMovementEvent MakeTerminal(
		const int64 WorldId,
		const ECorsairsMovementEventType Type,
		const FIntPoint Endpoint)
	{
		FCorsairsMovementEvent Event;
		Event.WorldId = WorldId;
		Event.Type = Type;
		Event.Waypoints = {Endpoint};
		Event.Endpoint = Endpoint;
		Event.bServerDriven = true;
		Event.MovementSpeedCmPerSecond = ServerSpeedCmPerSecond;
		return Event;
	}

	void InstallMovementRoutingSkill(
		UCorsairsSession* Session,
		const int64 SkillId)
	{
		Corsairs::Net::Msg::McSynSkillBagMessage Message;
		Message.worldId = LocalWorldId;
		Message.skillBag.synType = 0;
		Corsairs::Net::Msg::SkillEntry Skill;
		Skill.id = SkillId;
		Skill.level = 1;
		Message.skillBag.skills.push_back(Skill);
		WPacket Wire = Corsairs::Net::Msg::serialize(Message);
		RPacket Packet(Wire.Data(), Wire.GetPacketSize());
		Session->HandlePacketForTests(Packet);
	}

	ACorsairsCharacter* FindRemoteAt(
		UWorld* World,
		const ACorsairsPlayerCharacter* Local,
		const FIntPoint SourcePosition)
	{
		const FVector Expected(
			-static_cast<double>(SourcePosition.Y),
			static_cast<double>(SourcePosition.X),
			0.0);
		for (TActorIterator<ACorsairsCharacter> It(World); It; ++It)
		{
			if (*It != Local &&
				FMath::IsNearlyEqual((*It)->GetActorLocation().X, Expected.X) &&
				FMath::IsNearlyEqual((*It)->GetActorLocation().Y, Expected.Y))
			{
				return *It;
			}
		}
		return nullptr;
	}

	struct FMovementRoutingFixture
	{
		bool SetUp(FAutomationTestBase* Test)
		{
			if (!Test->TestTrue(
					TEXT("routing game world created"),
					TestWorld.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}

			UWorld* World = TestWorld.GetTestWorld();
			GameMode = World->SpawnActor<ACorsairsGameMode>(
				ACorsairsGameMode::StaticClass(),
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				MovementRoutingAlwaysSpawnParameters());
			Controller = World->SpawnActor<APlayerController>();
			Local = World->SpawnActor<ACorsairsPlayerCharacter>(
				ACorsairsPlayerCharacter::StaticClass(),
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				MovementRoutingAlwaysSpawnParameters());
			if (!Test->TestNotNull(TEXT("routing game mode spawned"), GameMode) ||
				!Test->TestNotNull(TEXT("routing controller spawned"), Controller) ||
				!Test->TestNotNull(TEXT("routing local pawn spawned"), Local))
			{
				return false;
			}

			GameMode->bAutoLogin = false;
			Controller->Possess(Local);
			if (!Test->TestTrue(
					TEXT("routing world begins play"),
					TestWorld.BeginPlayInTestWorld()))
			{
				TestWorld.ForwardErrorMessages(Test);
				return false;
			}
			if (!Test->TestTrue(
					TEXT("routing fixture startup is ready"),
					GameMode->IsStartupReady()))
			{
				TestWorld.ForwardErrorMessages(Test);
				return false;
			}

			Session = GameMode->GetSession();
			if (!Test->TestNotNull(TEXT("routing session exists"), Session))
			{
				return false;
			}
			Session->SetInWorldForTests(LocalWorldId, FIntPoint(0, 0));
			Session->SetMovementSpeedForTests(
				static_cast<int64>(ServerSpeedCmPerSecond));
			Local->AttachSession(Session);

			FString GroundError;
			// Один source-tile: (0..49,0..49)=10, (50..99,0..49)=20,
			// (0..49,50..99)=30, (50..99,50..99)=40 cm.
			const TArray<uint8> GroundBytes = {
				0x02, 0x04, 0x06, 0x08,
				0x02, 0x04, 0x06, 0x08,
				0x02, 0x04, 0x06, 0x08,
				0x02, 0x04, 0x06, 0x08,
			};
			const TArray<uint8> HeightBytes = {
				0x00, 0x80, 0x00, 0x80,
				0x00, 0x80, 0x00, 0x80,
			};
			const TArray<uint8> RegionBytes = {
				0x01, 0x00, 0x01, 0x00,
				0x01, 0x00, 0x01, 0x00,
			};
			if (!Test->TestTrue(
					TEXT("routing fixture loads navigation"),
					GameMode->LoadCharacterNavigationFromBytesForTests(
						2,
						2,
						HeightBytes,
						GroundBytes,
						RegionBytes,
						GroundError)))
			{
				TestWorld.ForwardErrorMessages(Test);
				return false;
			}
			GameMode->GroundCharacterForTests(Local, FIntPoint(0, 0));
			return true;
		}

		void ForwardErrors(FAutomationTestBase* Test)
		{
			TestWorld.ForwardErrorMessages(Test);
		}

		FTestWorldWrapper TestWorld;
		ACorsairsGameMode* GameMode = nullptr;
		APlayerController* Controller = nullptr;
		ACorsairsPlayerCharacter* Local = nullptr;
		UCorsairsSession* Session = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGameModeActorIncarnationReplacementTest,
	"Corsairs.Movement.Routing.ActorIncarnationReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGameModeActorIncarnationReplacementTest::RunTest(const FString&)
{
	// Мутации: Contains(WorldId) глотает новую инкарнацию и повтор той же
	// identity оставляет старые transform/metadata.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	FCorsairsWorldActor First;
	First.WorldId = 8801;
	First.Handle = 88001;
	First.Name = TEXT("First incarnation");
	First.Position = FIntPoint(20, 20);
	First.Angle = 15;
	First.TypeId = 1;
	First.CtrlType = 4;
	First.ChaId = 731;
	Fixture.GameMode->HandleActorSeenForTests(First);
	ACorsairsCharacter* OldRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, First.Position);
	if (!TestNotNull(TEXT("old incarnation exists"), OldRemote))
	{
		return false;
	}

	FCorsairsWorldActor Replacement = First;
	Replacement.Handle = 88002;
	Replacement.Name = TEXT("Second incarnation");
	Replacement.Position = FIntPoint(70, 20);
	Replacement.Angle = 95;
	Fixture.GameMode->HandleActorSeenForTests(Replacement);
	TestTrue(TEXT("new handle destroys the old UObject"),
		OldRemote->IsActorBeingDestroyed());
	TestEqual(TEXT("replacement keeps one entry in every registry"),
		Fixture.GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector(1, 1, 1));
	ACorsairsCharacter* NewRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, Replacement.Position);
	if (!TestNotNull(TEXT("new incarnation exists"), NewRemote))
	{
		return false;
	}
	TestTrue(TEXT("new incarnation uses another UObject"),
		NewRemote != OldRemote);
	FCorsairsServerIdentity Identity;
	TestTrue(TEXT("new incarnation exposes server identity"),
		NewRemote->TryGetServerIdentity(Identity));
	TestEqual(TEXT("new incarnation exposes replacement handle"),
		Identity.Handle, int64{88002});

	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGameModeInWorldReentryCleanupTest,
	"Corsairs.Movement.Routing.InWorldReentryCleansRemoteActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGameModeInWorldReentryCleanupTest::RunTest(const FString&)
{
	// Мутация: HandleStageChanged очищает remote registry только при выходе
	// из InWorld, поэтому ENTERMAP одной карты сразу в другую оставляет UObject.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	FCorsairsWorldActor Remote;
	Remote.WorldId = 8801;
	Remote.Handle = 88001;
	Remote.Name = TEXT("Previous map remote");
	Remote.Position = FIntPoint(20, 20);
	Remote.TypeId = 1;
	// Намеренно создаём registry-orphan без session DTO: только stage cleanup
	// способен убрать его при InWorld -> InWorld.
	Fixture.GameMode->HandleActorSeenForTests(Remote);
	ACorsairsCharacter* OldRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, Remote.Position);
	if (!TestNotNull(TEXT("previous-map remote exists"), OldRemote))
	{
		return false;
	}
	TestEqual(TEXT("orphan отсутствует в session visibility"),
		Fixture.Session->GetVisibleActors().Num(), 0);

	FCorsairsWorldActor Local;
	Local.WorldId = LocalWorldId;
	Local.Handle = 7701;
	Local.Name = TEXT("Reentered local");
	Local.Position = FIntPoint(0, 0);
	Local.TypeId = 1;
	Local.Look.TypeId = 1;
	Local.Look.HairId = 2000;
	Local.Look.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);
	Local.Look.EquipIds[1] = 255;
	Local.Look.EquipIds[2] = 289;
	Local.Look.EquipIds[3] = 465;
	Local.Look.EquipIds[4] = 641;
	Fixture.Session->SetInWorldAndBroadcastForTests(Local, TEXT("garner"));

	TestEqual(TEXT("InWorld to InWorld clears session visibility"),
		Fixture.Session->GetVisibleActors().Num(), 0);
	TestTrue(TEXT("InWorld to InWorld destroys previous-map UObject"),
		OldRemote->IsActorBeingDestroyed());
	TestEqual(TEXT("InWorld to InWorld empties all remote registries"),
		Fixture.GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector::ZeroValue);
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGameModeDynamicLifecycleTest,
	"Corsairs.Movement.Routing.GameModeDynamicLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGameModeDynamicLifecycleTest::RunTest(const FString&)
{
	// Мутация: не добавить динамический получатель GameMode, подписать его
	// дважды либо оставить получатель на сохранённой Session после EndPlay.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	FCorsairsWorldActor RemoteState;
	RemoteState.WorldId = 88;
	RemoteState.Handle = 8801;
	RemoteState.Name = TEXT("LifecycleRemote");
	RemoteState.Position = FIntPoint(0, 0);
	RemoteState.TypeId = 1;
	Fixture.GameMode->HandleActorSeenForTests(RemoteState);
	ACorsairsCharacter* Remote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, RemoteState.Position);
	if (!TestNotNull(TEXT("lifecycle remote is registered"), Remote))
	{
		return false;
	}

	TestEqual(TEXT("GameMode has exactly one movement binding"),
		CountMovementBindings(Fixture.Session, Fixture.GameMode), 1);
	const FVector BeforeMove = Remote->GetActorLocation();
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		RemoteState.WorldId,
		false,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(50, 0)}));
	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(TEXT("one real multicast advances mapped remote once"),
		Remote->GetActorLocation(),
		FVector(0.0, 20.0, BeforeMove.Z));

	UCorsairsSession* SavedSession = Fixture.Session;
	TestTrue(TEXT("GameMode accepts explicit destroy"), Fixture.GameMode->Destroy());
	TestEqual(TEXT("EndPlay removes GameMode movement binding"),
		CountMovementBindings(SavedSession, Fixture.GameMode), 0);
	TestTrue(TEXT("EndPlay destroys mapped remote"),
		Remote->IsActorBeingDestroyed());
	SavedSession->OnMovementChanged.Broadcast(MakeAcceptedPath(
		RemoteState.WorldId,
		false,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(90, 0)}));
	TestTrue(TEXT("saved-session broadcast leaves destroyed remote destroyed"),
		Remote->IsActorBeingDestroyed());
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGameModeControllerClassTest,
	"Corsairs.Movement.Routing.GameModeControllerClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGameModeControllerClassTest::RunTest(const FString&)
{
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	TestTrue(
		TEXT("GameMode использует точный игровой контроллер мыши"),
		Fixture.GameMode->PlayerControllerClass.Get() ==
			ACorsairsPlayerController::StaticClass());
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGameModeAppearanceFailureCleanupTest,
	"Corsairs.Movement.Routing.AppearanceFailureCleansUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGameModeAppearanceFailureCleanupTest::RunTest(const FString&)
{
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	FCorsairsWorldActor InvalidLocal;
	InvalidLocal.WorldId = LocalWorldId;
	InvalidLocal.Handle = 7701;
	InvalidLocal.Name = TEXT("InvalidAppearance");
	InvalidLocal.TypeId = MAX_int32;
	InvalidLocal.Position = FIntPoint(0, 0);
	AddExpectedError(
		TEXT("вход в мир остановлен"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Fixture.Session->SetInWorldAndBroadcastForTests(
		InvalidLocal,
		TEXT("garner"));
	TestEqual(
		TEXT("appearance failure is still in-world before deferred cleanup"),
		Fixture.Session->GetStage(),
		ECorsairsLoginStage::InWorld);

	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(
		TEXT("appearance failure logs the session out on the next tick"),
		Fixture.Session->GetStage(),
		ECorsairsLoginStage::Idle);
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterServerIdentityTest,
	"Corsairs.Movement.Routing.CharacterServerIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterServerIdentityTest::RunTest(const FString&)
{
	// Мутация: не привязать серверную identity к созданному персонажу либо
	// вернуть старое значение результата для персонажа, которого сервер не создавал.
	FMovementRoutingFixture fixture;
	if (!fixture.SetUp(this))
	{
		return false;
	}

	FCorsairsWorldActor remoteState;
	remoteState.WorldId = 8801;
	remoteState.Name = TEXT("IdentityRemote");
	remoteState.Position = FIntPoint(20, 20);
	remoteState.TypeId = 1;
	remoteState.CtrlType = 4;
	remoteState.ChaId = 731;
	remoteState.Handle = 990011;
	fixture.GameMode->HandleActorSeenForTests(remoteState);
	ACorsairsCharacter* remote = Cast<ACorsairsCharacter>(FindRemoteAt(
		fixture.TestWorld.GetTestWorld(), fixture.Local, remoteState.Position));
	if (!TestNotNull(TEXT("server-created character exists"), remote))
	{
		return false;
	}

	FCorsairsServerIdentity identity;
	TestTrue(TEXT("server-created character exposes identity"),
		remote->TryGetServerIdentity(identity));
	TestEqual(TEXT("identity preserves world id"),
		identity.WorldId, static_cast<int64>(8801));
	TestEqual(TEXT("identity preserves handle"),
		identity.Handle, static_cast<int64>(990011));
	TestEqual(TEXT("identity preserves control type"), identity.CtrlType, 4);
	TestEqual(TEXT("identity preserves character id"), identity.ChaId, 731);
	TestTrue(TEXT("exact identity initialization is idempotent"),
		remote->InitializeServerIdentity(identity));
	FCorsairsServerIdentity otherIdentity = identity;
	otherIdentity.Handle = 990012;
	TestFalse(TEXT("different identity cannot replace the first one"),
		remote->InitializeServerIdentity(otherIdentity));
	TestTrue(TEXT("rejected replacement preserves identity"),
		remote->TryGetServerIdentity(identity));
	TestEqual(TEXT("rejected replacement preserves handle"),
		identity.Handle, static_cast<int64>(990011));

	ACorsairsCharacter* unbound =
		fixture.TestWorld.GetTestWorld()->SpawnActor<ACorsairsCharacter>(
			ACorsairsCharacter::StaticClass(),
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			MovementRoutingAlwaysSpawnParameters());
	if (!TestNotNull(TEXT("unbound character exists"), unbound))
	{
		return false;
	}
	identity.WorldId = 1;
	identity.Handle = 2;
	identity.CtrlType = 3;
	identity.ChaId = 4;
	TestFalse(TEXT("unbound character has no server identity"),
		unbound->TryGetServerIdentity(identity));
	TestEqual(TEXT("failed lookup clears world id"), identity.WorldId, int64{0});
	TestEqual(TEXT("failed lookup clears handle"), identity.Handle, int64{0});
	TestEqual(TEXT("failed lookup clears control type"), identity.CtrlType, 0);
	TestEqual(TEXT("failed lookup clears character id"), identity.ChaId, 0);
	FCorsairsServerIdentity invalidIdentity;
	TestFalse(TEXT("zero world id cannot initialize identity"),
		unbound->InitializeServerIdentity(invalidIdentity));
	invalidIdentity.WorldId = 55;
	TestFalse(TEXT("zero handle cannot initialize identity"),
		unbound->InitializeServerIdentity(invalidIdentity));

	const FIntVector registriesBeforeInvalid =
		fixture.GameMode->GetRemoteRegistryCountsForTests();
	FCorsairsWorldActor invalidRemoteState = remoteState;
	invalidRemoteState.WorldId = 0;
	invalidRemoteState.Position = FIntPoint(30, 30);
	fixture.GameMode->HandleActorSeenForTests(invalidRemoteState);
	TestEqual(TEXT("invalid server identity is not registered"),
		fixture.GameMode->GetRemoteRegistryCountsForTests(),
		registriesBeforeInvalid);
	ACorsairsCharacter* InvalidRemote = FindRemoteAt(
		fixture.TestWorld.GetTestWorld(),
		fixture.Local,
		invalidRemoteState.Position);
	TestTrue(
		TEXT("invalid server identity leaves no live world actor"),
		InvalidRemote == nullptr || InvalidRemote->IsActorBeingDestroyed());
	invalidRemoteState.WorldId = 8802;
	invalidRemoteState.Handle = 0;
	invalidRemoteState.Position = FIntPoint(40, 40);
	fixture.GameMode->HandleActorSeenForTests(invalidRemoteState);
	TestEqual(TEXT("zero-handle actor is not registered"),
		fixture.GameMode->GetRemoteRegistryCountsForTests(),
		registriesBeforeInvalid);
	InvalidRemote = FindRemoteAt(
		fixture.TestWorld.GetTestWorld(),
		fixture.Local,
		invalidRemoteState.Position);
	TestTrue(TEXT("zero-handle actor is destroyed before publication"),
		InvalidRemote == nullptr || InvalidRemote->IsActorBeingDestroyed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsLocalDeliveredOnceTest,
	"Corsairs.Movement.Routing.LocalDeliveredOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsLocalDeliveredOnceTest::RunTest(const FString&)
{
	// Мутация: GameMode пересылает локальные события, pawn начинает manual
	// path либо skill path игнорирует скорость события и authority lock.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	TestEqual(TEXT("pawn has one direct movement receiver"),
		CountMovementBindings(Fixture.Session, Fixture.Local), 1);
	TestEqual(TEXT("GameMode has one separate remote-only receiver"),
		CountMovementBindings(Fixture.Session, Fixture.GameMode), 1);
	const FVector Start = Fixture.Local->GetActorLocation();
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		LocalWorldId,
		true,
		false,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(80, 0)}));
	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(TEXT("click AcceptedPath двигает pawn с серверной скоростью"),
		Fixture.Local->GetActorLocation(), FVector(0.0, 20.0, Start.Z));
	TestEqual(TEXT("click path входит в follower ровно один раз"),
		Fixture.Local->GetServerPathAcceptCountForTests(), 1);

	FCorsairsWorldActor Target;
	Target.WorldId = 99;
	Target.Position = FIntPoint(80, 0);
	Target.Handle = 9099;
	Fixture.Session->AddVisibleActorForTests(Target);
	InstallMovementRoutingSkill(Fixture.Session, 26);
	Fixture.Session->SetSendOverrideForTests([](WPacket&) { return true; });
	TestEqual(TEXT("skill reserves server authority"),
		static_cast<uint8>(Fixture.Session->UseSkillOn(26, Target.WorldId)),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestTrue(TEXT("skill lock remains active before server path"),
		Fixture.Session->IsMovementAuthorityLocked());

	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		LocalWorldId,
		true,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(50, 0)}));
	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(TEXT("one local server path transition advances by event speed"),
		Fixture.Local->GetActorLocation(),
		FVector(0.0, 20.0, Start.Z));
	TestEqual(TEXT("skill path повторно входит в follower ровно один раз"),
		Fixture.Local->GetServerPathAcceptCountForTests(), 2);
	TestTrue(TEXT("skill lock stays active while follower moves"),
		Fixture.Session->IsMovementAuthorityLocked());
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsRemoteByWorldIdTest,
	"Corsairs.Movement.Routing.RemoteByWorldId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsRemoteByWorldIdTest::RunTest(const FString&)
{
	// Мутация: маршрутизировать по любому actor вместо Event.WorldId, повторно
	// использовать неверную speed, сломать ground-преобразование waypoint или
	// не синхронизировать rejection.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	FCorsairsWorldActor First;
	First.WorldId = 101;
	First.Handle = 10101;
	First.Name = TEXT("FirstRemote");
	First.Position = FIntPoint(10, 10);
	First.TypeId = 1;
	FCorsairsWorldActor Second;
	Second.WorldId = 202;
	Second.Handle = 20202;
	Second.Name = TEXT("SecondRemote");
	Second.Position = FIntPoint(20, 20);
	Second.Angle = 123;
	Second.TypeId = 1;
	Fixture.GameMode->HandleActorSeenForTests(First);
	Fixture.GameMode->HandleActorSeenForTests(Second);
	ACorsairsCharacter* FirstRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, First.Position);
	ACorsairsCharacter* SecondRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, Second.Position);
	if (!TestNotNull(TEXT("first mapped remote exists"), FirstRemote) ||
		!TestNotNull(TEXT("second mapped remote exists"), SecondRemote))
	{
		return false;
	}

	const FVector FirstBefore = FirstRemote->GetActorLocation();
	TestEqual(TEXT("second remote starts at its distinct first ground height"),
		SecondRemote->GetActorLocation(), FVector(-20.0, 20.0, 98.0));
	TestTrue(TEXT("remote actor preserves source angle semantics"),
		FMath::IsNearlyEqual(SecondRemote->GetActorRotation().Yaw, 123.0));
	TestEqual(TEXT("second remote starts with capsule bottom at 10 cm"),
		SecondRemote->GetActorLocation().Z -
			SecondRemote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		10.0);
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		Second.WorldId,
		false,
		true,
		ServerSpeedCmPerSecond,
		{Second.Position, FIntPoint(70, 20)}));
	Fixture.TestWorld.TickTestWorld(0.25f);
	TestEqual(TEXT("unaddressed remote stays at its accepted position"),
		FirstRemote->GetActorLocation(), FirstBefore);
	TestEqual(TEXT("addressed remote follows waypoint at 20 cm ground height"),
		SecondRemote->GetActorLocation(),
		FVector(-20.0, 70.0, 108.0));
	TestEqual(TEXT("accepted waypoint preserves 20 cm capsule bottom"),
		SecondRemote->GetActorLocation().Z -
			SecondRemote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		20.0);
	TestTrue(TEXT("remote path applies rigid-basis facing"),
		FMath::IsNearlyEqual(SecondRemote->GetActorRotation().Yaw, 90.0));

	const FIntPoint RejectedEndpoint(30, 70);
	Fixture.Session->OnMovementChanged.Broadcast(MakeTerminal(
		Second.WorldId,
		ECorsairsMovementEventType::Rejected,
		RejectedEndpoint));
	TestEqual(TEXT("remote rejection reconciles into 30 cm ground cell"),
		SecondRemote->GetActorLocation(),
		FVector(-70.0, 30.0, 118.0));
	TestEqual(TEXT("rejected endpoint preserves 30 cm capsule bottom"),
		SecondRemote->GetActorLocation().Z -
			SecondRemote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		30.0);
	Fixture.TestWorld.TickTestWorld(0.5f);
	TestEqual(TEXT("rejected remote no longer replays old path"),
		SecondRemote->GetActorLocation(),
		FVector(-70.0, 30.0, 118.0));

	const FVector BeforeZeroSpeed = SecondRemote->GetActorLocation();
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		Second.WorldId,
		false,
		true,
		0.0,
		{RejectedEndpoint, FIntPoint(90, 30)}));
	Fixture.TestWorld.TickTestWorld(0.5f);
	TestEqual(TEXT("zero speed never starts remote playback"),
		SecondRemote->GetActorLocation(), BeforeZeroSpeed);
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsUngroundedCharacterRigidBasisTest,
	"Corsairs.Movement.Routing.UngroundedCharacterRigidBasis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsUngroundedCharacterRigidBasisTest::RunTest(const FString&)
{
	// Мутация: в fallback-ветке без CharacterGround вернуть
	// старое отражение F=(x,-y) вместо rigid Q=(-y,x).
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("ungrounded character world created"), World))
	{
		return false;
	}

	ACorsairsCharacter* Character = World->SpawnActor<ACorsairsCharacter>(
		ACorsairsCharacter::StaticClass(),
		FVector(999.0, 999.0, 321.0),
		FRotator::ZeroRotator,
		MovementRoutingAlwaysSpawnParameters());
	if (!TestNotNull(TEXT("ungrounded character spawned"), Character))
	{
		World->DestroyWorld(false);
		return false;
	}

	Character->HandleServerMovementChanged(MakeAcceptedPath(
		303,
		false,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(10, 20), FIntPoint(60, 20)}));
	TestEqual(TEXT("ungrounded path start uses rigid basis and keeps Z"),
		Character->GetActorLocation(), FVector(-20.0, 10.0, 321.0));

	Character->Tick(0.1f);
	TestEqual(TEXT("ungrounded path advance uses rigid basis and keeps Z"),
		Character->GetActorLocation(), FVector(-20.0, 30.0, 321.0));
	TestTrue(TEXT("ungrounded path applies rigid-basis facing"),
		FMath::IsNearlyEqual(Character->GetActorRotation().Yaw, 90.0));

	World->DestroyWorld(false);
	return true;
}

#endif
