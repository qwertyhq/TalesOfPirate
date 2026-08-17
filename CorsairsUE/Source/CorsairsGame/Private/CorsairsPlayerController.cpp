#include "CorsairsPlayerController.h"

#include "CorsairsCameraProfile.h"
#include "CorsairsCharacter.h"
#include "CorsairsCharacterGround.h"
#include "CorsairsGroundPicker.h"
#include "CorsairsLoginHud.h"
#include "CorsairsPlayerCharacter.h"
#include "CorsairsSkillCatalog.h"
#include "CorsairsSkillHudPresentation.h"
#include "Components/InputComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "InputCoreTypes.h"

#include <chrono>

namespace
{
constexpr int32 FightShortcutType = 2;
constexpr double NpcTalkDistance = 300.0;
constexpr double NpcApproachDistance = 200.0;
constexpr double TraceDistanceCm = 10000000.0;
constexpr double CameraYawSensitivity = 1.0;
constexpr int64 ArriveMoveState = 1;

const TArray<FKey>& ShortcutKeys()
{
	static const TArray<FKey> Keys = {
		EKeys::F1,
		EKeys::F2,
		EKeys::F3,
		EKeys::F4,
		EKeys::F5,
		EKeys::F6,
		EKeys::F7,
		EKeys::F8,
		EKeys::F9,
		EKeys::F10,
		EKeys::F11,
		EKeys::F12,
	};
	return Keys;
}

double SourceDistance(const FIntPoint Left, const FIntPoint Right)
{
	const FVector2D Delta(
		static_cast<double>(Left.X) - static_cast<double>(Right.X),
		static_cast<double>(Left.Y) - static_cast<double>(Right.Y));
	return Delta.Size();
}

bool IsSkillIntent(const ECorsairsClickIntentType Type)
{
	return Type == ECorsairsClickIntentType::EntitySkill ||
		Type == ECorsairsClickIntentType::GroundSkill;
}

FIntPoint NavigationCell(const FIntPoint Point)
{
	return FIntPoint(
		FMath::FloorToInt(
			static_cast<double>(Point.X) /
			static_cast<double>(FCorsairsMapPathfinder::CellSize)),
		FMath::FloorToInt(
			static_cast<double>(Point.Y) /
		static_cast<double>(FCorsairsMapPathfinder::CellSize)));
}

FIntPoint NavigationCellCenter(const FIntPoint Point)
{
	const FIntPoint Cell = NavigationCell(Point);
	return FIntPoint(
		Cell.X * FCorsairsMapPathfinder::CellSize +
			FCorsairsMapPathfinder::CellCenterOffset,
		Cell.Y * FCorsairsMapPathfinder::CellSize +
			FCorsairsMapPathfinder::CellCenterOffset);
}

bool IsSameNavigationCell(const FIntPoint Left, const FIntPoint Right)
{
	return NavigationCell(Left) == NavigationCell(Right);
}
} // namespace

ACorsairsPlayerController::ACorsairsPlayerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
	PrimaryActorTick.bCanEverTick = true;
}

void ACorsairsPlayerController::BeginPlay()
{
	Super::BeginPlay();
	ApplyFreeCursorMode();
}

void ACorsairsPlayerController::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	AttachGameplay(nullptr, nullptr, nullptr);
	Super::EndPlay(EndPlayReason);
}

void ACorsairsPlayerController::PlayerTick(const float DeltaSeconds)
{
	Super::PlayerTick(DeltaSeconds);
	if (_session != nullptr && _ground != nullptr &&
		_mouseState.ConsumeHeldWalkSample(std::chrono::steady_clock::now()))
	{
		ResolveCursorClick(ECorsairsPathMode::StraightOnly, true);
	}
}

void ACorsairsPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (InputComponent == nullptr || _inputBindingsInstalled)
	{
		return;
	}
	_inputBindingsInstalled = true;

	InputComponent->BindKey(
		EKeys::LeftMouseButton,
		IE_Pressed,
		this,
		&ACorsairsPlayerController::HandleLeftPressed);
	InputComponent->BindKey(
		EKeys::LeftMouseButton,
		IE_Released,
		this,
		&ACorsairsPlayerController::HandleLeftReleased);
	InputComponent->BindKey(
		EKeys::RightMouseButton,
		IE_Pressed,
		this,
		&ACorsairsPlayerController::HandleRightPressed);
	InputComponent->BindKey(
		EKeys::RightMouseButton,
		IE_Released,
		this,
		&ACorsairsPlayerController::HandleRightReleased);
	InputComponent->BindKey(
		EKeys::RightMouseButton,
		IE_DoubleClick,
		this,
		&ACorsairsPlayerController::HandleRightDoubleClick);
	InputComponent->BindAxisKey(
		EKeys::MouseX,
		this,
		&ACorsairsPlayerController::HandleMouseX);
	InputComponent->BindAxisKey(
		EKeys::MouseY,
		this,
		&ACorsairsPlayerController::HandleMouseY);
	InputComponent->BindAxisKey(
		EKeys::MouseWheelAxis,
		this,
		&ACorsairsPlayerController::HandleMouseWheel);

	const TArray<FKey>& Keys = ShortcutKeys();
	for (int32 Slot = 0; Slot < Keys.Num(); ++Slot)
	{
		FInputKeyBinding Binding(
			FInputChord(Keys[Slot], false, false, false, false),
			IE_Pressed);
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda(
			[this, Slot]() { HandleShortcut(Slot); });
		InputComponent->KeyBindings.Emplace(MoveTemp(Binding));
	}

	TArray<FKey> TypedKeys;
	for (TCHAR Letter = 'A'; Letter <= 'Z'; ++Letter)
	{
		TypedKeys.Add(FKey(*FString::Chr(Letter)));
	}
	for (TCHAR Digit = '0'; Digit <= '9'; ++Digit)
	{
		TypedKeys.Add(FKey(*FString::Chr(Digit)));
	}
	TypedKeys.Append({EKeys::BackSpace, EKeys::Tab, EKeys::Enter});
	for (const FKey& Key : TypedKeys)
	{
		FInputKeyBinding Binding(
			FInputChord(Key, false, false, false, false),
			IE_Pressed);
		Binding.bConsumeInput = false;
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda(
			[this, Key]() { HandleTypedKey(Key); });
		InputComponent->KeyBindings.Emplace(MoveTemp(Binding));
	}
}

void ACorsairsPlayerController::AttachGameplay(
	UCorsairsSession* InSession,
	const FCorsairsCharacterGround* InGround,
	const FCorsairsSkillCatalog* InSkillCatalog)
{
	if (_session != nullptr)
	{
		_session->OnMovementChanged.RemoveDynamic(
			this,
			&ACorsairsPlayerController::HandleMovementChanged);
		_session->OnActorLeft.RemoveDynamic(
			this,
			&ACorsairsPlayerController::HandleActorLeft);
		_session->OnStageChanged.RemoveDynamic(
			this,
			&ACorsairsPlayerController::HandleStageChanged);
		_session->OnSkillStateChanged.RemoveDynamic(
			this,
			&ACorsairsPlayerController::HandleSkillStateChanged);
		_session->OnProtocolError.RemoveDynamic(
			this,
			&ACorsairsPlayerController::HandleProtocolError);
	}

	_session = InSession;
	_ground = InGround;
	_skillCatalog = InSkillCatalog;
	ResetGameplayState();
	ApplyFreeCursorMode();
	if (_session == nullptr || _ground == nullptr || _skillCatalog == nullptr ||
		!_ground->IsNavigationLoaded())
	{
		_session = nullptr;
		_ground = nullptr;
		_skillCatalog = nullptr;
		return;
	}

	_session->OnMovementChanged.AddDynamic(
		this,
		&ACorsairsPlayerController::HandleMovementChanged);
	_session->OnActorLeft.AddDynamic(
		this,
		&ACorsairsPlayerController::HandleActorLeft);
	_session->OnStageChanged.AddDynamic(
		this,
		&ACorsairsPlayerController::HandleStageChanged);
	_session->OnSkillStateChanged.AddDynamic(
		this,
		&ACorsairsPlayerController::HandleSkillStateChanged);
	_session->OnProtocolError.AddDynamic(
		this,
		&ACorsairsPlayerController::HandleProtocolError);
}

bool ACorsairsPlayerController::BuildSkillHudPresentation(
	FCorsairsSkillHudPresentation& Out) const
{
	if (_session == nullptr || _skillCatalog == nullptr ||
		_session->GetStage() != ECorsairsLoginStage::InWorld)
	{
		Out = FCorsairsSkillHudPresentation{};
		return false;
	}
	Out = MakeCorsairsSkillHudPresentation(
		_session->GetShortcuts(),
		_session->GetSkillBag(),
		*_skillCatalog,
		_preparedSkillId,
		_selectedTargetName,
		_actionFeedback);
	return true;
}

void ACorsairsPlayerController::ApplyFreeCursorMode()
{
	bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	if (GetWorld() != nullptr && GetWorld()->GetGameViewport() != nullptr)
	{
		GetWorld()->GetGameViewport()->SetMouseCaptureMode(
			EMouseCaptureMode::NoCapture);
		GetWorld()->GetGameViewport()->SetMouseLockMode(
			EMouseLockMode::DoNotLock);
	}
}

void ACorsairsPlayerController::ApplyCapturedCursorMode()
{
	bShowMouseCursor = false;
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(true);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::LockOnCapture);
	SetInputMode(InputMode);
	if (GetWorld() != nullptr && GetWorld()->GetGameViewport() != nullptr)
	{
		GetWorld()->GetGameViewport()->SetMouseCaptureMode(
			EMouseCaptureMode::CaptureDuringRightMouseDown);
		GetWorld()->GetGameViewport()->SetMouseLockMode(
			EMouseLockMode::LockOnCapture);
		GetWorld()->GetGameViewport()->SetHideCursorDuringCapture(true);
	}
}

bool ACorsairsPlayerController::IsLoginUiAcceptingInput() const
{
	const ACorsairsLoginHud* LoginHud = Cast<ACorsairsLoginHud>(GetHUD());
	return LoginHud != nullptr && LoginHud->IsAcceptingInput();
}

void ACorsairsPlayerController::HandleTypedKey(const FKey Key)
{
	ACorsairsLoginHud* LoginHud = Cast<ACorsairsLoginHud>(GetHUD());
	if (LoginHud == nullptr || !LoginHud->IsAcceptingInput())
	{
		return;
	}
	if (Key == EKeys::BackSpace)
	{
		LoginHud->EraseCharacter();
		return;
	}
	if (Key == EKeys::Tab)
	{
		LoginHud->NextField();
		return;
	}
	if (Key == EKeys::Enter)
	{
		LoginHud->SubmitLogin();
		return;
	}
	const FString Name = Key.GetFName().ToString();
	if (Name.Len() == 1)
	{
		LoginHud->AppendCharacter(Name.ToLower());
	}
}

void ACorsairsPlayerController::HandleLeftPressed()
{
	if (IsLoginUiAcceptingInput())
	{
		return;
	}
	_mouseState.BeginLeft(std::chrono::steady_clock::now());
	ResolveCursorClick(ECorsairsPathMode::Normal, false);
}

void ACorsairsPlayerController::HandleLeftReleased()
{
	_mouseState.EndLeft();
}

void ACorsairsPlayerController::HandleRightPressed()
{
	if (IsLoginUiAcceptingInput())
	{
		return;
	}
	double MouseX = 0.0;
	double MouseY = 0.0;
	GetMousePosition(MouseX, MouseY);
	_mouseState.BeginRight(
		FVector2D(MouseX, MouseY),
		std::chrono::steady_clock::now());
	ApplyCapturedCursorMode();
}

void ACorsairsPlayerController::HandleRightReleased()
{
	const FCorsairsRightRelease Release =
		_mouseState.EndRight(std::chrono::steady_clock::now());
	ApplyFreeCursorMode();
	if (!Release.bHadPress)
	{
		return;
	}
	SetMouseLocation(
		FMath::RoundToInt(Release.RestoreCursor.X),
		FMath::RoundToInt(Release.RestoreCursor.Y));
	if (Release.bCancelIntent)
	{
		CancelTargeting(true);
	}
}

void ACorsairsPlayerController::HandleRightDoubleClick()
{
	if (IsLoginUiAcceptingInput())
	{
		return;
	}
	const auto Profile = Corsairs::Game::Camera::LegacyDefaultProfile();
	const FRotator Current = GetControlRotation();
	SetControlRotation(FRotator(
		Current.Pitch,
		Profile.InitialYawDegrees,
		0.0));
}

void ACorsairsPlayerController::HandleMouseX(const float Value)
{
	if (IsLoginUiAcceptingInput() || !_mouseState.IsRightHeld() ||
		!FMath::IsFinite(Value) ||
		FMath::IsNearlyZero(Value))
	{
		return;
	}
	_mouseState.AddRightDrag(FVector2D(Value, 0.0));
	const FRotator Current = GetControlRotation();
	SetControlRotation(FRotator(
		Current.Pitch,
		Current.Yaw + Value * CameraYawSensitivity,
		0.0));
}

void ACorsairsPlayerController::HandleMouseY(const float Value)
{
	if (!IsLoginUiAcceptingInput() && _mouseState.IsRightHeld() &&
		FMath::IsFinite(Value))
	{
		_mouseState.AddRightDrag(FVector2D(0.0, Value));
	}
}

void ACorsairsPlayerController::HandleMouseWheel(const float Value)
{
	if (IsLoginUiAcceptingInput() || !FMath::IsFinite(Value) ||
		FMath::IsNearlyZero(Value))
	{
		return;
	}
	const auto Profile = Corsairs::Game::Camera::LegacyDefaultProfile();
	_cameraZoom = Corsairs::Game::Camera::ApplyWheel(
		Profile,
		_cameraZoom,
		static_cast<double>(Value));
	if (ACorsairsPlayerCharacter* PlayerPawn =
		Cast<ACorsairsPlayerCharacter>(GetPawn()))
	{
		PlayerPawn->ApplyCameraZoom(_cameraZoom);
	}
}

void ACorsairsPlayerController::HandleShortcut(const int32 Slot)
{
	if (_session == nullptr || _skillCatalog == nullptr ||
		_session->GetStage() != ECorsairsLoginStage::InWorld ||
		Slot < 0 || Slot >= 12)
	{
		return;
	}
	const FCorsairsShortcutEntry* Shortcut =
		_session->GetShortcuts().FindByPredicate(
			[Slot](const FCorsairsShortcutEntry& Entry)
			{
				return Entry.Slot == Slot;
			});
	if (Shortcut == nullptr || Shortcut->Type != FightShortcutType ||
		Shortcut->GridId <= 0)
	{
		SetFeedback(TEXT("ячейка не содержит боевой навык"));
		return;
	}
	if (FindUsableSkill(Shortcut->GridId) == nullptr ||
		_skillCatalog->Find(Shortcut->GridId) == nullptr)
	{
		SetFeedback(TEXT("навык ячейки недоступен"));
		return;
	}

	_preparedSkillId = Shortcut->GridId;
	SetFeedback(FString());
}

bool ACorsairsPlayerController::BuildCursorSnapshot(
	FCorsairsClickInput& OutInput) const
{
	OutInput = FCorsairsClickInput{};
	if (_session == nullptr || _ground == nullptr || _skillCatalog == nullptr)
	{
		return false;
	}
	OutInput.bLoginGate =
		_session->GetStage() != ECorsairsLoginStage::InWorld;
	OutInput.bUiConsumed = IsLoginUiAcceptingInput();
	OutInput.LocalActor = _session->GetLocalActor();
	OutInput.SkillBag = _session->GetSkillBag();
	OutInput.PreparedSkillId = _preparedSkillId;
	if (_preparedSkillId != 0)
	{
		if (const FCorsairsSkillDefinition* Skill =
			_skillCatalog->Find(_preparedSkillId))
		{
			OutInput.PreparedSkill = *Skill;
		}
	}
	OutInput.DefaultSkillId = _session->GetDefaultSkillId();
	if (OutInput.DefaultSkillId != 0)
	{
		if (const FCorsairsSkillDefinition* Skill =
			_skillCatalog->Find(OutInput.DefaultSkillId))
		{
			OutInput.DefaultSkill = *Skill;
		}
	}

	FVector RayOrigin;
	FVector RayDirection;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return false;
	}
	FCorsairsWorldActor ExactActor;
	if (TraceExactActor(RayOrigin, RayDirection, ExactActor))
	{
		OutInput.bHasExactActor = true;
		OutInput.ExactActor = ExactActor;
		OutInput.bHasExactActorIdentity = true;
		OutInput.ExactActorWorldId = ExactActor.WorldId;
		OutInput.ExactActorHandle = ExactActor.Handle;
	}
	FIntPoint GroundPoint;
	if (FCorsairsGroundPicker::TryPick(
			RayOrigin,
			RayDirection,
			*_ground,
			GroundPoint))
	{
		OutInput.bHasGroundPoint = true;
		OutInput.GroundPoint = GroundPoint;
	}
	return true;
}

bool ACorsairsPlayerController::TraceExactActor(
	const FVector& RayOrigin,
	const FVector& RayDirection,
	FCorsairsWorldActor& OutActor) const
{
	UWorld* World = GetWorld();
	if (World == nullptr || RayOrigin.ContainsNaN() || RayDirection.ContainsNaN() ||
		RayDirection.IsNearlyZero())
	{
		return false;
	}

	FCollisionObjectQueryParams ObjectQuery;
	ObjectQuery.AddObjectTypesToQuery(ECC_Pawn);
	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(CorsairsMouseActorTrace),
		true);
	QueryParams.AddIgnoredActor(GetPawn());
	TArray<FHitResult> Hits;
	World->LineTraceMultiByObjectType(
		Hits,
		RayOrigin,
		RayOrigin + RayDirection.GetSafeNormal() * TraceDistanceCm,
		ObjectQuery,
		QueryParams);
	for (const FHitResult& Hit : Hits)
	{
		const ACorsairsCharacter* Character =
			Cast<ACorsairsCharacter>(Hit.GetActor());
		if (Character == nullptr)
		{
			continue;
		}
		FCorsairsServerIdentity Identity;
		if (Character->TryGetServerIdentity(Identity) &&
			TryFindExactActor(Identity.WorldId, Identity.Handle, OutActor))
		{
			return true;
		}
	}
	return false;
}

bool ACorsairsPlayerController::TryFindExactActor(
	const int64 WorldId,
	const int64 Handle,
	FCorsairsWorldActor& OutActor) const
{
	if (_session == nullptr || WorldId == 0 || Handle == 0)
	{
		return false;
	}
	const FCorsairsWorldActor Local = _session->GetLocalActor();
	if (Local.WorldId == WorldId && Local.Handle == Handle)
	{
		OutActor = Local;
		return true;
	}
	for (const FCorsairsWorldActor& Actor : _session->GetVisibleActors())
	{
		if (Actor.WorldId == WorldId && Actor.Handle == Handle)
		{
			OutActor = Actor;
			return true;
		}
	}
	return false;
}

const FCorsairsSkillEntry* ACorsairsPlayerController::FindUsableSkill(
	const int64 SkillId) const
{
	if (_session == nullptr)
	{
		return nullptr;
	}
	return _session->GetSkillBag().FindByPredicate(
		[SkillId](const FCorsairsSkillEntry& Entry)
		{
			return Entry.SkillId == SkillId && Entry.Level > 0;
		});
}

void ACorsairsPlayerController::ResolveCursorClick(
	const ECorsairsPathMode PathMode,
	const bool bHeldWalkOnly)
{
	FCorsairsClickInput Input;
	if (!BuildCursorSnapshot(Input))
	{
		SetFeedback(TEXT("точка под курсором недоступна"));
		return;
	}
	if (bHeldWalkOnly)
	{
		if (!Input.bHasGroundPoint)
		{
			return;
		}
		FCorsairsClickIntent Intent;
		Intent.Type = ECorsairsClickIntentType::Move;
		Intent.GroundPoint = Input.GroundPoint;
		SubmitIntent(Intent, PathMode);
		return;
	}

	const FCorsairsClickIntent Intent =
		FCorsairsWorldClickResolver::Resolve(Input);
	if (!Input.bUiConsumed && !Input.bLoginGate &&
		Input.bHasExactActor && Input.bHasExactActorIdentity &&
		Input.ExactActor.WorldId == Input.ExactActorWorldId &&
		Input.ExactActor.Handle == Input.ExactActorHandle)
	{
		_selectedTargetName = Input.ExactActor.Name;
		_selectedTargetWorldId = Input.ExactActor.WorldId;
		_selectedTargetHandle = Input.ExactActor.Handle;
	}
	else if (_preparedSkillId == 0)
	{
		_selectedTargetName.Empty();
		_selectedTargetWorldId = 0;
		_selectedTargetHandle = 0;
	}
	FCorsairsMouseTarget Target;
	bool bHasTarget = false;
	const int64 ActionId = Intent.SkillId != 0
		? Intent.SkillId
		: -static_cast<int64>(Intent.Type) - 1;
	if (Intent.TargetWorldId != 0 && Intent.TargetHandle != 0)
	{
		Target = FCorsairsMouseTarget::Actor(
			Intent.TargetWorldId,
			Intent.TargetHandle,
			ActionId);
		bHasTarget = true;
	}
	else if (Input.bHasGroundPoint)
	{
		Target = FCorsairsMouseTarget::Ground(Input.GroundPoint, ActionId);
		bHasTarget = true;
	}
	if (bHasTarget && !_mouseState.AcceptTarget(
			Target,
			std::chrono::steady_clock::now()))
	{
		return;
	}
	SubmitIntent(Intent, PathMode);
}

void ACorsairsPlayerController::SubmitIntent(
	const FCorsairsClickIntent& Intent,
	const ECorsairsPathMode PathMode)
{
	if (_session == nullptr)
	{
		return;
	}
	_pendingNpc.Reset();
	_continuationIntent.Reset();
	if (Intent.Type == ECorsairsClickIntentType::Rejected ||
		Intent.Type == ECorsairsClickIntentType::SelectOnly)
	{
		_pendingReplacement.Reset();
		SetFeedback(Intent.Reason);
		return;
	}

	const FCorsairsQueuedClickIntent Queued{Intent, PathMode};
	if (_session->HasActiveAction())
	{
		_pendingReplacement = Queued;
		if (!_session->IsCancelPending())
		{
			_session->EndActiveAction();
		}
		if (!_session->HasActiveAction() && _pendingReplacement.IsSet())
		{
			const FCorsairsQueuedClickIntent Ready =
				_pendingReplacement.GetValue();
			_pendingReplacement.Reset();
			ExecuteIntent(Ready);
		}
		return;
	}
	ExecuteIntent(Queued);
}

void ACorsairsPlayerController::ExecuteIntent(
	const FCorsairsQueuedClickIntent& Queued)
{
	if (_session == nullptr || _ground == nullptr)
	{
		return;
	}
	const FCorsairsClickIntent& Intent = Queued.Intent;
	if (Intent.Type == ECorsairsClickIntentType::Move)
	{
		SendMovementToward(Intent.GroundPoint, Queued.PathMode, Queued);
		return;
	}
	if (Intent.Type == ECorsairsClickIntentType::Talk)
	{
		ExecuteTalk(Intent);
		return;
	}

	FIntPoint ApproachTarget = Intent.GroundPoint;
	FCorsairsWorldActor TargetActor;
	if (Intent.Type == ECorsairsClickIntentType::EntitySkill)
	{
		if (!TryFindExactActor(
				Intent.TargetWorldId,
				Intent.TargetHandle,
				TargetActor))
		{
			SetFeedback(TEXT("цель навыка исчезла"));
			return;
		}
		ApproachTarget = TargetActor.Position;
		_selectedTargetName = TargetActor.Name;
		_selectedTargetWorldId = TargetActor.WorldId;
		_selectedTargetHandle = TargetActor.Handle;
	}

	FCorsairsPathResult PathResult;
	TArray<FIntPoint> Path = BuildApproachPath(
		ApproachTarget,
		Queued.PathMode,
		PathResult);
	if (IsSkillIntent(Intent.Type) && Path.IsEmpty() &&
		PathResult.ResolvedTarget != FIntPoint::ZeroValue &&
		IsSameNavigationCell(
			_session->GetConfirmedPosition(),
			PathResult.ResolvedTarget))
	{
		// Pathfinder правильно возвращает no-op для одной half-cell, но SKILL
		// wire обязан содержать две legacy-точки в центрах навигационных ячеек.
		const FIntPoint CellCenter = NavigationCellCenter(
			_session->GetConfirmedPosition());
		Path = {
			CellCenter,
			CellCenter,
		};
	}
	else if (IsSkillIntent(Intent.Type) && Path.Num() == 1)
	{
		// Защита для частичного straight-prefix: SKILL wire требует минимум
		// две точки даже для уже укороченного пути.
		Path.Add(Path[0]);
	}
	if (Path.Num() < 2)
	{
		SetFeedback(TEXT("к цели нельзя построить путь"));
		return;
	}
	if (PathResult.bTruncated)
	{
		if (_session->SendMovePath(Path) == ECorsairsActionRequestResult::Sent)
		{
			_continuationIntent = Queued;
			SetFeedback(FString());
		}
		else
		{
			SetFeedback(TEXT("путь к цели не отправлен"));
		}
		return;
	}

	ECorsairsActionRequestResult Result =
		ECorsairsActionRequestResult::Invalid;
	if (Intent.Type == ECorsairsClickIntentType::EntitySkill)
	{
		Result = _session->UseSkillOn(
			Intent.SkillId,
			Intent.TargetWorldId,
			Intent.TargetHandle,
			Path);
	}
	else if (Intent.Type == ECorsairsClickIntentType::GroundSkill)
	{
		Result = _session->UseSkillAtPoint(
			Intent.SkillId,
			Intent.GroundPoint,
			Path);
	}
	if (Result == ECorsairsActionRequestResult::Sent)
	{
		if (_preparedSkillId == Intent.SkillId)
		{
			_preparedSkillId = 0;
		}
		SetFeedback(FString());
	}
	else if (Result == ECorsairsActionRequestResult::Busy)
	{
		_pendingReplacement = Queued;
	}
	else
	{
		SetFeedback(TEXT("серверный запрос навыка не отправлен"));
	}
}

bool ACorsairsPlayerController::SendMovementToward(
	const FIntPoint Target,
	const ECorsairsPathMode PathMode,
	const TOptional<FCorsairsQueuedClickIntent>& Continuation)
{
	if (_session == nullptr || _ground == nullptr)
	{
		return false;
	}
	FCorsairsPathResult PathResult;
	const TArray<FIntPoint> Path = BuildApproachPath(
		Target,
		PathMode,
		PathResult);
	if (Path.Num() < 2)
	{
		SetFeedback(TEXT("для движения нет безопасного пути"));
		return false;
	}
	const ECorsairsActionRequestResult Result = _session->SendMovePath(Path);
	if (Result != ECorsairsActionRequestResult::Sent)
	{
		SetFeedback(TEXT("путь не отправлен серверу"));
		return false;
	}
	if (PathResult.bTruncated && Continuation.IsSet())
	{
		_continuationIntent = Continuation;
	}
	SetFeedback(FString());
	return true;
}

TArray<FIntPoint> ACorsairsPlayerController::BuildApproachPath(
	const FIntPoint Target,
	const ECorsairsPathMode PathMode,
	FCorsairsPathResult& OutResult) const
{
	if (_session == nullptr || _ground == nullptr)
	{
		OutResult = FCorsairsPathResult{};
		return {};
	}
	OutResult = FCorsairsMapPathfinder::FindPath(
		*_ground,
		_session->GetConfirmedPosition(),
		Target,
		PathMode,
		ECorsairsTraversalKind::Land);
	return OutResult.Waypoints;
}

void ACorsairsPlayerController::ExecuteTalk(
	const FCorsairsClickIntent& Intent)
{
	FCorsairsWorldActor Npc;
	if (_session == nullptr || !TryFindExactActor(
			Intent.TargetWorldId,
			Intent.TargetHandle,
			Npc))
	{
		SetFeedback(TEXT("NPC исчез"));
		return;
	}
	_selectedTargetName = Npc.Name;
	_selectedTargetWorldId = Npc.WorldId;
	_selectedTargetHandle = Npc.Handle;
	const FIntPoint Current = _session->GetConfirmedPosition();
	if (SourceDistance(Current, Npc.Position) <= NpcTalkDistance)
	{
		// Между разрешением клика и отправкой персонаж мог уйти и вернуться с
		// новым Handle. Разговор адресуется WorldId, поэтому пара сверяется снова.
		FCorsairsWorldActor CurrentNpc;
		if (!TryFindExactActor(Npc.WorldId, Npc.Handle, CurrentNpc))
		{
			SetFeedback(TEXT("NPC identity изменилась"));
			return;
		}
		if (!_session->TalkToNpc(Npc.WorldId))
		{
			SetFeedback(TEXT("разговор с NPC не отправлен"));
		}
		else
		{
			SetFeedback(FString());
		}
		return;
	}

	const FVector2D FromNpc(
		static_cast<double>(Current.X - Npc.Position.X),
		static_cast<double>(Current.Y - Npc.Position.Y));
	const FVector2D Direction = FromNpc.GetSafeNormal();
	const FIntPoint Approach(
		FMath::RoundToInt(
			static_cast<double>(Npc.Position.X) +
			Direction.X * NpcApproachDistance),
		FMath::RoundToInt(
			static_cast<double>(Npc.Position.Y) +
			Direction.Y * NpcApproachDistance));
	_pendingNpc = FCorsairsPendingNpcInteraction{
		Npc.WorldId,
		Npc.Handle,
	};
	const FCorsairsQueuedClickIntent Continuation{
		Intent,
		ECorsairsPathMode::Normal,
	};
	if (!SendMovementToward(
			Approach,
			ECorsairsPathMode::Normal,
			Continuation))
	{
		_pendingNpc.Reset();
	}
}

void ACorsairsPlayerController::CancelTargeting(
	const bool bRequestServerCancel)
{
	_preparedSkillId = 0;
	_pendingReplacement.Reset();
	_continuationIntent.Reset();
	_pendingNpc.Reset();
	_selectedTargetName.Empty();
	_selectedTargetWorldId = 0;
	_selectedTargetHandle = 0;
	SetFeedback(FString());
	if (bRequestServerCancel && _session != nullptr &&
		_session->HasActiveAction() && !_session->IsCancelPending())
	{
		_session->EndActiveAction();
	}
}

void ACorsairsPlayerController::ResetGameplayState()
{
	_mouseState.Reset();
	_pendingReplacement.Reset();
	_continuationIntent.Reset();
	_pendingNpc.Reset();
	_preparedSkillId = 0;
	_selectedTargetName.Empty();
	_selectedTargetWorldId = 0;
	_selectedTargetHandle = 0;
	_actionFeedback.Empty();
}

void ACorsairsPlayerController::SetFeedback(const FString& Message)
{
	_actionFeedback = Message;
}

void ACorsairsPlayerController::HandleMovementChanged(
	const FCorsairsMovementEvent& Event)
{
	if (!Event.bLocal || Event.Type == ECorsairsMovementEventType::AcceptedPath)
	{
		return;
	}
	if (_pendingReplacement.IsSet())
	{
		const FCorsairsQueuedClickIntent Ready =
			_pendingReplacement.GetValue();
		_pendingReplacement.Reset();
		ExecuteIntent(Ready);
		return;
	}
	const bool bArrived =
		Event.Type == ECorsairsMovementEventType::Terminal &&
		Event.MoveState == ArriveMoveState;
	if (!bArrived)
	{
		_pendingReplacement.Reset();
		_pendingNpc.Reset();
		_continuationIntent.Reset();
		SetFeedback(
			Event.Type == ECorsairsMovementEventType::Rejected
				? TEXT("сервер отклонил движение")
				: TEXT("движение завершилось без ARRIVE"));
		return;
	}
	if (_continuationIntent.IsSet())
	{
		const FCorsairsQueuedClickIntent Ready =
			_continuationIntent.GetValue();
		_continuationIntent.Reset();
		_pendingNpc.Reset();
		ExecuteIntent(Ready);
		return;
	}
	if (_pendingNpc.IsSet())
	{
		const FCorsairsPendingNpcInteraction Pending = _pendingNpc.GetValue();
		_pendingNpc.Reset();
		FCorsairsWorldActor Npc;
		if (TryFindExactActor(Pending.WorldId, Pending.Handle, Npc) &&
			SourceDistance(_session->GetConfirmedPosition(), Npc.Position) <=
				NpcTalkDistance)
		{
			if (!_session->TalkToNpc(Pending.WorldId))
			{
				SetFeedback(TEXT("разговор с NPC не отправлен"));
			}
		}
		else
		{
			SetFeedback(TEXT("NPC больше не доступен"));
		}
	}
}

void ACorsairsPlayerController::HandleActorLeft(const int64 WorldId)
{
	if (_selectedTargetWorldId == WorldId)
	{
		_selectedTargetName.Empty();
		_selectedTargetWorldId = 0;
		_selectedTargetHandle = 0;
	}
	if (_pendingNpc.IsSet() && _pendingNpc->WorldId == WorldId)
	{
		_pendingNpc.Reset();
		SetFeedback(TEXT("NPC вышел из поля зрения"));
	}
	if (_pendingReplacement.IsSet() &&
		_pendingReplacement->Intent.TargetWorldId == WorldId)
	{
		_pendingReplacement.Reset();
	}
	if (_continuationIntent.IsSet() &&
		_continuationIntent->Intent.TargetWorldId == WorldId)
	{
		_continuationIntent.Reset();
	}
}

void ACorsairsPlayerController::HandleStageChanged(
	const ECorsairsLoginStage Stage,
	const FString& Message)
{
	if (Stage != ECorsairsLoginStage::InWorld)
	{
		ResetGameplayState();
		if (Stage == ECorsairsLoginStage::Failed)
		{
			SetFeedback(Message);
		}
	}
}

void ACorsairsPlayerController::HandleSkillStateChanged()
{
	if (_preparedSkillId != 0 && FindUsableSkill(_preparedSkillId) == nullptr)
	{
		_preparedSkillId = 0;
		SetFeedback(TEXT("подготовленный навык удалён из skill bag"));
	}
}

void ACorsairsPlayerController::HandleProtocolError(const FString& Message)
{
	SetFeedback(Message);
}

#if WITH_DEV_AUTOMATION_TESTS
void ACorsairsPlayerController::PressRightMouseForTests(
	const FVector2D Cursor)
{
	_mouseState.BeginRight(Cursor, std::chrono::steady_clock::now());
	ApplyCapturedCursorMode();
}

void ACorsairsPlayerController::ReleaseRightMouseForTests(
	const bool bUnusedAdvanceTime)
{
	(void)bUnusedAdvanceTime;
	HandleRightReleased();
}

bool ACorsairsPlayerController::RotateCameraForTests(const float MouseX)
{
	const FRotator Before = GetControlRotation();
	HandleMouseX(MouseX);
	return !GetControlRotation().Equals(Before, 0.001);
}

void ACorsairsPlayerController::ResetCameraYawForTests()
{
	HandleRightDoubleClick();
}

void ACorsairsPlayerController::ApplyWheelForTests(const float WheelDelta)
{
	HandleMouseWheel(WheelDelta);
}

void ACorsairsPlayerController::SelectActorForTests(
	const FCorsairsWorldActor& Actor)
{
	_selectedTargetName = Actor.Name;
	_selectedTargetWorldId = Actor.WorldId;
	_selectedTargetHandle = Actor.Handle;
}

void ACorsairsPlayerController::SubmitIntentForTests(
	const FCorsairsClickIntent& Intent,
	const ECorsairsPathMode PathMode)
{
	SubmitIntent(Intent, PathMode);
}
#endif
