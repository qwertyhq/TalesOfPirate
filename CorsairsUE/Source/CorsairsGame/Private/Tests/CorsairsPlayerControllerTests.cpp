#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsPlayerController.h"

#include "CorsairsCameraProfile.h"
#include "CorsairsCharacterGround.h"
#include "CorsairsPlayerCharacter.h"
#include "CorsairsSkillCatalog.h"
#include "CorsairsSession.h"
#include "CorsairsNet/include/CommandMessages.h"
#include "CorsairsNet/include/Packet.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/SpringArmComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

namespace
{
TArray<uint8> FlatNavigation(const int32 Count, const bool bHeight)
{
	TArray<uint8> Bytes;
	if (bHeight)
	{
		Bytes.Reserve(Count * 2);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Bytes.Append({0x00, 0x80});
		}
	}
	else
	{
		Bytes.Init(0, Count * 4);
	}
	return Bytes;
}

TArray<uint8> LandNavigation(const int32 Count)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Count * 2);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Bytes.Append({0x01, 0x00});
	}
	return Bytes;
}

FString WriteControllerCatalogFixture()
{
	const FString Path = FPaths::ProjectSavedDir() /
		TEXT("PlayerControllerSkills.json");
	FFileHelper::SaveStringToFile(
		TEXT(R"json({"schemaVersion":1,"skills":[]})json"),
		*Path);
	return Path;
}

FString WriteControllerSkillCatalogFixture()
{
	const FString Path = FPaths::ProjectSavedDir() /
		TEXT("PlayerControllerSkill.json");
	FFileHelper::SaveStringToFile(
		TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":11,"name":"Удар","applyDistance":100,"applyTarget":1,"applyType":1,"helpful":false,"habitatMask":0,"radius":0,"shape":0,"targetMode":"entity"}]})json"),
		*Path);
	return Path;
}

void InstallControllerSkill(UCorsairsSession* Session, const int64 WorldId)
{
	Corsairs::Net::Msg::McSynSkillBagMessage Message;
	Message.worldId = WorldId;
	Message.skillBag.synType = 0;
	Corsairs::Net::Msg::SkillEntry Skill;
	Skill.id = 11;
	Skill.level = 1;
	Message.skillBag.skills.push_back(Skill);
	Corsairs::Net::WPacket Wire =
		Corsairs::Net::Msg::serialize(Message);
	Corsairs::Net::RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}

template <typename MessageType>
void DeliverControllerMessage(
	UCorsairsSession* Session,
	const MessageType& Message)
{
	Corsairs::Net::WPacket Wire = Corsairs::Net::Msg::serialize(Message);
	Corsairs::Net::RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerControllerMouseCameraTest,
	"Corsairs.Input.PlayerController.MouseCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerControllerMouseCameraTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(
			TEXT("создан игровой мир контроллера"),
			TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	if (World == nullptr || !TestWorld.BeginPlayInTestWorld())
	{
		TestWorld.ForwardErrorMessages(this);
		return false;
	}

	ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
	ACorsairsPlayerController* Controller =
		World->SpawnActor<ACorsairsPlayerController>();
	ACorsairsPlayerCharacter* Pawn =
		World->SpawnActor<ACorsairsPlayerCharacter>();
	if (!TestNotNull(TEXT("создан local player"), LocalPlayer) ||
		!TestNotNull(TEXT("создан mouse controller"), Controller) ||
		!TestNotNull(TEXT("создан player pawn"), Pawn))
	{
		return false;
	}
	Controller->SetPlayer(LocalPlayer);
	Controller->InitInputSystem();
	Controller->Possess(Pawn);

	TestTrue(TEXT("свободный курсор видим"), Controller->bShowMouseCursor);
	TestFalse(TEXT("свободный MouseX не вращает камеру"),
		Controller->RotateCameraForTests(15.0f));
	const FRotator BeforeDrag = Controller->GetControlRotation();
	Controller->PressRightMouseForTests(FVector2D(120.0, 80.0));
	TestFalse(TEXT("ПКМ скрывает курсор"), Controller->bShowMouseCursor);
	TestTrue(TEXT("MouseX при ПКМ принят"),
		Controller->RotateCameraForTests(15.0f));
	TestTrue(TEXT("ПКМ меняет только yaw"),
		FMath::IsNearlyEqual(
			Controller->GetControlRotation().Yaw,
			BeforeDrag.Yaw + 15.0f,
			0.01f));
	TestTrue(TEXT("MouseY не меняет pitch"), FMath::IsNearlyEqual(
		Controller->GetControlRotation().Pitch, BeforeDrag.Pitch, 0.01f));
	Controller->ReleaseRightMouseForTests(false);
	TestTrue(TEXT("release ПКМ возвращает курсор"),
		Controller->bShowMouseCursor);

	Controller->SetControlRotation(FRotator(BeforeDrag.Pitch, 73.0, 0.0));
	Controller->ResetCameraYawForTests();
	TestTrue(TEXT("double RMB возвращает legacy yaw"), FMath::IsNearlyEqual(
		Controller->GetControlRotation().Yaw,
		static_cast<float>(
			Corsairs::Game::Camera::LegacyDefaultProfile().InitialYawDegrees),
		0.01f));

	USpringArmComponent* Boom = Pawn->FindComponentByClass<USpringArmComponent>();
	if (TestNotNull(TEXT("spring arm существует"), Boom))
	{
		Controller->ApplyWheelForTests(100.0f);
		const double MinArm = Corsairs::Game::Camera::DeriveRig(
			Corsairs::Game::Camera::LegacyDefaultProfile(), 16.0 / 9.0, 0.0)
			.ArmLengthCm;
		TestTrue(TEXT("wheel жёстко ограничен профилем"),
			FMath::IsNearlyEqual(
				Boom->TargetArmLength,
				static_cast<float>(MinArm),
				0.01f));
	}

	TestWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerControllerInputBindingsTest,
	"Corsairs.Input.PlayerController.Bindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerControllerInputBindingsTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(TEXT("создан мир input binding"),
			TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	if (World == nullptr || !TestWorld.BeginPlayInTestWorld())
	{
		return false;
	}
	ACorsairsPlayerController* Controller =
		World->SpawnActor<ACorsairsPlayerController>();
	ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
	if (!TestNotNull(TEXT("создан контроллер binding"), Controller) ||
		!TestNotNull(TEXT("создан local player binding"), LocalPlayer))
	{
		return false;
	}
	Controller->SetPlayer(LocalPlayer);
	Controller->InitInputSystem();
	Controller->SetupInputComponent();
	Controller->SetupInputComponent();
	UInputComponent* Input = Controller->FindComponentByClass<UInputComponent>();
	if (!TestNotNull(TEXT("input component создан"), Input))
	{
		return false;
	}
	const auto CountKey = [Input](const FKey Key, const EInputEvent Event)
	{
		int32 Count = 0;
		for (const FInputKeyBinding& Binding : Input->KeyBindings)
		{
			if (Binding.Chord.Key == Key && Binding.KeyEvent == Event)
			{
				++Count;
			}
		}
		return Count;
	};
	TestEqual(TEXT("ЛКМ pressed привязан ровно один раз"),
		CountKey(EKeys::LeftMouseButton, IE_Pressed), 1);
	TestEqual(TEXT("ЛКМ released привязан ровно один раз"),
		CountKey(EKeys::LeftMouseButton, IE_Released), 1);
	TestEqual(TEXT("ПКМ pressed привязан ровно один раз"),
		CountKey(EKeys::RightMouseButton, IE_Pressed), 1);
	TestEqual(TEXT("ПКМ released привязан ровно один раз"),
		CountKey(EKeys::RightMouseButton, IE_Released), 1);
	TestEqual(TEXT("ПКМ double click привязан ровно один раз"),
		CountKey(EKeys::RightMouseButton, IE_DoubleClick), 1);
	const TArray<FKey> ShortcutKeys = {
		EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4,
		EKeys::F5, EKeys::F6, EKeys::F7, EKeys::F8,
		EKeys::F9, EKeys::F10, EKeys::F11, EKeys::F12,
	};
	for (int32 Slot = 0; Slot < ShortcutKeys.Num(); ++Slot)
	{
			TestEqual(
				FString::Printf(TEXT("F%d привязан"), Slot + 1),
				CountKey(ShortcutKeys[Slot], IE_Pressed), 1);
	}
	FCorsairsWorldActor Selected;
	Selected.WorldId = 700;
	Selected.Handle = 701;
	Selected.Name = TEXT("Точная цель");
	Controller->SelectActorForTests(Selected);
	TestEqual(TEXT("exact selection хранит имя"),
		Controller->GetSelectedTargetName(), Selected.Name);
	TestEqual(TEXT("exact selection хранит WorldId"),
		Controller->GetSelectedTargetWorldId(), Selected.WorldId);
	TestEqual(TEXT("exact selection хранит Handle"),
		Controller->GetSelectedTargetHandle(), Selected.Handle);
	Controller->ActorLeftForTests(Selected.WorldId);
	TestTrue(TEXT("ActorLeft очищает имя exact target"),
		Controller->GetSelectedTargetName().IsEmpty());
	TestEqual(TEXT("ActorLeft очищает WorldId exact target"),
		Controller->GetSelectedTargetWorldId(), int64{0});
	TestEqual(TEXT("ActorLeft очищает Handle exact target"),
		Controller->GetSelectedTargetHandle(), int64{0});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerControllerLatestReplacementAfterCancelTest,
	"Corsairs.Input.PlayerController.LatestReplacementAfterCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerControllerLatestReplacementAfterCancelTest::RunTest(
	const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(TEXT("создан мир замены движения"),
			TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	if (World == nullptr || !TestWorld.BeginPlayInTestWorld())
	{
		return false;
	}
	ACorsairsPlayerController* Controller =
		World->SpawnActor<ACorsairsPlayerController>();
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	if (!TestNotNull(TEXT("создан контроллер замены"), Controller) ||
		!TestNotNull(TEXT("создана сессия замены"), Session))
	{
		return false;
	}

	const FIntPoint Spawn(25, 25);
	Session->SetInWorldForTests(77, Spawn);
	int32 MoveSendCount = 0;
	int32 CancelSendCount = 0;
	FIntPoint LastMoveEndpoint = FIntPoint::ZeroValue;
	Session->SetSendOverrideForTests(
		[&](Corsairs::Net::WPacket& Wire)
		{
			if (Wire.GetCmd() == CMD_CM_ENDACTION)
			{
				++CancelSendCount;
				return true;
			}
			if (Wire.GetCmd() != CMD_CM_BEGINACTION)
			{
				return true;
			}

			++MoveSendCount;
			Corsairs::Net::RPacket Packet(
				Wire.Data(),
				Wire.GetPacketSize());
			Packet.ReadInt64();
			Packet.ReadInt64();
			Packet.ReadInt64();
			uint16 PathBytes = 0;
			const char* Path = Packet.ReadSequence(PathBytes);
			if (Path != nullptr && PathBytes >= sizeof(int32) * 2)
			{
				const char* Endpoint =
					Path + PathBytes - sizeof(int32) * 2;
				FMemory::Memcpy(
					&LastMoveEndpoint.X,
					Endpoint,
					sizeof(int32));
				FMemory::Memcpy(
					&LastMoveEndpoint.Y,
					Endpoint + sizeof(int32),
					sizeof(int32));
			}
			return true;
		});

	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("navigation замены загружена"), Ground.LoadRuntimeFromBytes(
		4,
		2,
		FlatNavigation(8, true),
		FlatNavigation(8, false),
		LandNavigation(8),
		Error));
	FCorsairsSkillCatalog Catalog;
	TestTrue(TEXT("каталог замены загружен"),
		Catalog.Load(WriteControllerCatalogFixture(), Error));
	Controller->AttachGameplay(Session, &Ground, &Catalog);

	FCorsairsClickIntent Move;
	Move.Type = ECorsairsClickIntentType::Move;
	Move.GroundPoint = FIntPoint(125, 25);
	Controller->SubmitIntentForTests(Move);
	TestEqual(TEXT("первый клик отправляет MOVE"), MoveSendCount, 1);
	TestEqual(TEXT("первый MOVE идёт к первой точке"),
		LastMoveEndpoint, FIntPoint(125, 25));

	Move.GroundPoint = FIntPoint(225, 25);
	Controller->SubmitIntentForTests(Move);
	TestEqual(TEXT("второй клик отправляет один ENDACTION"),
		CancelSendCount, 1);
	TestEqual(TEXT("замена не отправляется до terminal"), MoveSendCount, 1);

	Move.GroundPoint = FIntPoint(325, 25);
	Controller->SubmitIntentForTests(Move);
	TestEqual(TEXT("третий клик не дублирует ENDACTION"),
		CancelSendCount, 1);
	TestEqual(TEXT("последняя замена всё ещё ждёт terminal"), MoveSendCount, 1);

	// В настоящем маршруте reducer очищает active action до уведомления.
	Session->SetInWorldForTests(77, Spawn);
	FCorsairsMovementEvent CancelTerminal;
	CancelTerminal.bLocal = true;
	CancelTerminal.Type = ECorsairsMovementEventType::Terminal;
	CancelTerminal.MoveState = 2;
	Controller->HandleMovementForTests(CancelTerminal);
	TestEqual(TEXT("cancel terminal запускает последний MOVE"),
		MoveSendCount, 2);
	TestEqual(TEXT("после отмены отправлена точка третьего клика"),
		LastMoveEndpoint, FIntPoint(325, 25));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerControllerNpcTerminalGateTest,
	"Corsairs.Input.PlayerController.NpcTalkRequiresArrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerControllerNpcTerminalGateTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(TEXT("создан мир NPC interaction"),
			TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	if (World == nullptr || !TestWorld.BeginPlayInTestWorld())
	{
		return false;
	}
	ACorsairsPlayerController* Controller =
		World->SpawnActor<ACorsairsPlayerController>();
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	if (!TestNotNull(TEXT("создан NPC controller"), Controller) ||
		!TestNotNull(TEXT("создана NPC session"), Session))
	{
		return false;
	}
	Session->SetInWorldForTests(77, FIntPoint(25, 25));
	FCorsairsWorldActor Npc;
	Npc.WorldId = 900;
	Npc.Handle = 901;
	Npc.Name = TEXT("Pappa");
	Npc.CtrlType = 2;
	Npc.Position = FIntPoint(375, 25);
	Session->AddVisibleActorForTests(Npc);
	int32 MovePackets = 0;
	int32 TalkPackets = 0;
	int64 TalkWorldId = 0;
	int64 TalkAction = 0;
	int64 TalkPage = -1;
	Session->SetSendOverrideForTests(
		[&](Corsairs::Net::WPacket& Wire)
		{
			if (Wire.GetCmd() == CMD_CM_BEGINACTION)
			{
				++MovePackets;
			}
			else if (Wire.GetCmd() == CMD_CM_REQUESTNPC)
			{
				++TalkPackets;
				Corsairs::Net::RPacket Packet(
					Wire.Data(),
					Wire.GetPacketSize());
				TalkWorldId = Packet.ReadInt64();
				TalkAction = Packet.ReadInt64();
				TalkPage = Packet.ReadInt64();
			}
			return true;
		});

	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("NPC navigation загружена"), Ground.LoadRuntimeFromBytes(
		4,
		2,
		FlatNavigation(8, true),
		FlatNavigation(8, false),
		LandNavigation(8),
		Error));
	FCorsairsSkillCatalog Catalog;
	TestTrue(TEXT("пустой каталог NPC загружен"),
		Catalog.Load(WriteControllerCatalogFixture(), Error));
	Controller->AttachGameplay(Session, &Ground, &Catalog);

	FCorsairsClickIntent Talk;
	Talk.Type = ECorsairsClickIntentType::Talk;
	Talk.TargetWorldId = Npc.WorldId;
	Talk.TargetHandle = Npc.Handle;
	Controller->SubmitIntentForTests(Talk);
	TestTrue(TEXT("дальний NPC ждёт подход"),
		Controller->HasPendingNpcForTests());
	TestEqual(TEXT("подход отправил один MOVE"), MovePackets, 1);
	TestEqual(TEXT("до ARRIVE разговор не отправлен"), TalkPackets, 0);

	const FIntPoint Approach(175, 25);
	Session->SetInWorldForTests(77, Approach);
	Session->AddVisibleActorForTests(Npc);
	FCorsairsMovementEvent Arrived;
	Arrived.bLocal = true;
	Arrived.Type = ECorsairsMovementEventType::Terminal;
	Arrived.MoveState = 1;
	Arrived.Endpoint = Approach;
	Controller->HandleMovementForTests(Arrived);
	TestFalse(TEXT("ARRIVE очищает ожидающий разговор"),
		Controller->HasPendingNpcForTests());
	TestEqual(TEXT("ARRIVE отправляет ровно один TALK"), TalkPackets, 1);
	TestEqual(TEXT("TALK адресован точному NPC"), TalkWorldId, Npc.WorldId);
	TestEqual(TEXT("TALK содержит legacy действие TALKPAGE"),
		TalkAction, int64{302});
	TestEqual(TEXT("TALK открывает начальную страницу"), TalkPage, int64{0});

	Session->SetInWorldForTests(77, FIntPoint(25, 25));
	Session->AddVisibleActorForTests(Npc);
	Controller->SubmitIntentForTests(Talk);
	TestTrue(TEXT("повторный дальний NPC снова ждёт подход"),
		Controller->HasPendingNpcForTests());
	TestEqual(TEXT("повторный подход отправляет второй MOVE"), MovePackets, 2);

	FCorsairsMovementEvent Blocked;
	Blocked.bLocal = true;
	Blocked.Type = ECorsairsMovementEventType::Rejected;
	Blocked.MoveState = 2;
	Controller->HandleMovementForTests(Blocked);
	TestFalse(TEXT("BLOCK очищает ожидающий разговор"),
		Controller->HasPendingNpcForTests());
	TestEqual(TEXT("BLOCK не отправляет второй TALK"), TalkPackets, 1);

	Session->SetInWorldForTests(77, FIntPoint(25, 25));
	Session->AddVisibleActorForTests(Npc);
	Controller->SubmitIntentForTests(Talk);
	TestTrue(TEXT("третий подход ждёт NPC"),
		Controller->HasPendingNpcForTests());
	FCorsairsClickIntent SelectOnly;
	SelectOnly.Type = ECorsairsClickIntentType::SelectOnly;
	SelectOnly.Reason = TEXT("выбрана другая цель");
	Controller->SubmitIntentForTests(SelectOnly);
	TestFalse(TEXT("новый select-only отменяет старый NPC intent"),
		Controller->HasPendingNpcForTests());
	Controller->HandleMovementForTests(Arrived);
	TestEqual(TEXT("ARRIVE после select-only не повторяет TALK"),
		TalkPackets, 1);

	Session->SetInWorldForTests(77, FIntPoint(25, 25));
	Corsairs::Net::Msg::McChaBeginSeeMessage FirstNpc;
	FirstNpc.seeType = 1;
	FirstNpc.base.worldId = Npc.WorldId;
	FirstNpc.base.handle = Npc.Handle;
	FirstNpc.base.ctrlType = 2;
	FirstNpc.base.name = "Pappa";
	FirstNpc.base.posX = Npc.Position.X;
	FirstNpc.base.posY = Npc.Position.Y;
	FirstNpc.base.look.typeId = 1;
	DeliverControllerMessage(Session, FirstNpc);
	Controller->SubmitIntentForTests(Talk);
	TestTrue(TEXT("подход к первой инкарнации NPC ожидается"),
		Controller->HasPendingNpcForTests());
	TestEqual(TEXT("выбрана первая инкарнация NPC"),
		Controller->GetSelectedTargetHandle(), Npc.Handle);
	Controller->SeedContinuationForTests(Talk);
	TestTrue(TEXT("продолжение старой инкарнации подготовлено"),
		Controller->HasContinuationForTests());

	Corsairs::Net::Msg::McChaBeginSeeMessage ReplacementNpc = FirstNpc;
	ReplacementNpc.base.handle = 902;
	ReplacementNpc.base.name = "Pappa replacement";
	DeliverControllerMessage(Session, ReplacementNpc);
	TestFalse(TEXT("новая инкарнация отменяет ожидающий TALK"),
		Controller->HasPendingNpcForTests());
	TestFalse(TEXT("новая инкарнация очищает продолжение подхода"),
		Controller->HasContinuationForTests());
	TestEqual(TEXT("новая инкарнация очищает старый target handle"),
		Controller->GetSelectedTargetHandle(), int64{0});
	Controller->HandleMovementForTests(Arrived);
	TestEqual(TEXT("ARRIVE старой инкарнации не отправляет TALK"),
		TalkPackets, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerControllerSkillSameCellFallbackTest,
	"Corsairs.Input.PlayerController.SkillSameCellFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerControllerSkillSameCellFallbackTest::RunTest(
	const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(TEXT("создан мир SKILL fallback"),
			TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	if (World == nullptr || !TestWorld.BeginPlayInTestWorld())
	{
		return false;
	}
	ACorsairsPlayerController* Controller =
		World->SpawnActor<ACorsairsPlayerController>();
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	if (!TestNotNull(TEXT("создан SKILL controller"), Controller) ||
		!TestNotNull(TEXT("создана SKILL session"), Session))
	{
		return false;
	}

	constexpr int64 WorldId = 77;
	Session->SetInWorldForTests(WorldId, FIntPoint(45, 45));
	InstallControllerSkill(Session, WorldId);
	FCorsairsWorldActor Target;
	Target.WorldId = 900;
	Target.Handle = 901;
	Target.Name = TEXT("Цель");
	Target.Position = FIntPoint(35, 35);
	Session->AddVisibleActorForTests(Target);

	bool bWireMatches = false;
	Session->SetSendOverrideForTests(
		[&bWireMatches](Corsairs::Net::WPacket& Wire)
		{
			Corsairs::Net::RPacket Packet(
				Wire.Data(),
				Wire.GetPacketSize());
			Corsairs::Net::Msg::CmBeginActionMessage Message;
			Corsairs::Net::Msg::deserialize(Packet, Message);
			const auto* Skill =
				std::get_if<Corsairs::Net::Msg::CmActionSkillInputData>(
					&Message.data);
			const FIntPoint Expected[] = {
				FIntPoint(25, 25),
				FIntPoint(25, 25),
			};
			bWireMatches =
				Message.actionType ==
					Corsairs::Net::Msg::ActionType::SKILL &&
				Skill != nullptr && Skill->chMove == 2 &&
				Skill->skillId == 11 &&
				Skill->tarInfo1 == 900 && Skill->tarInfo2 == 901 &&
				Skill->pathData.size() == sizeof(Expected) &&
				FMemory::Memcmp(
					Skill->pathData.data(), Expected, sizeof(Expected)) == 0;
			return true;
		});

	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("SKILL navigation загружена"), Ground.LoadRuntimeFromBytes(
		2,
		2,
		FlatNavigation(4, true),
		FlatNavigation(4, false),
		LandNavigation(4),
		Error));
	FCorsairsSkillCatalog Catalog;
	TestTrue(TEXT("SKILL каталог загружен"),
		Catalog.Load(WriteControllerSkillCatalogFixture(), Error));
	Controller->AttachGameplay(Session, &Ground, &Catalog);

	FCorsairsClickIntent SkillIntent;
	SkillIntent.Type = ECorsairsClickIntentType::EntitySkill;
	SkillIntent.SkillId = 11;
	SkillIntent.TargetWorldId = Target.WorldId;
	SkillIntent.TargetHandle = Target.Handle;
	Controller->SubmitIntentForTests(SkillIntent);
	TestTrue(TEXT("same-cell SKILL отправляет две legacy-точки"),
		bWireMatches);
	return true;
}

#endif
