#pragma once

#include "CoreMinimal.h"

#include "CorsairsMapPathfinder.h"
#include "CorsairsMouseControlState.h"
#include "CorsairsWorldClickResolver.h"
#include "GameFramework/PlayerController.h"

#include "CorsairsPlayerController.generated.h"

class ACorsairsCharacter;
class ACorsairsLoginHud;
class ACorsairsPlayerCharacter;
class FCorsairsCharacterGround;
class FCorsairsSkillCatalog;
struct FCorsairsSkillHudPresentation;
class UCorsairsSession;

/** Полное намерение для повторного разрешения после ответа сервера. */
struct FCorsairsQueuedClickIntent
{
	FCorsairsClickIntent Intent;
	ECorsairsPathMode PathMode = ECorsairsPathMode::Normal;
};

/** Точное взаимодействие с NPC, ожидающее завершения подхода. */
struct FCorsairsPendingNpcInteraction
{
	int64 WorldId = 0;
	int64 Handle = 0;
};

/**
 * Единственный владелец gameplay-ввода мыши.
 *
 * Контроллер переводит точный cursor ray в actor/ground intent, но не решает
 * за сервер исход движения или навыка. Сессия остаётся владельцем wire и
 * reducer lifecycle, а здесь хранится только последнее пользовательское
 * намерение для replan после terminal.
 */
UCLASS()
class CORSAIRSGAME_API ACorsairsPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ACorsairsPlayerController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PlayerTick(float DeltaSeconds) override;
	virtual void SetupInputComponent() override;

	/** Подключает серверное игровое состояние после успешной активации мира. */
	void AttachGameplay(
		UCorsairsSession* InSession,
		const FCorsairsCharacterGround* InGround,
		const FCorsairsSkillCatalog* InSkillCatalog);

	int64 GetPreparedSkillId() const { return _preparedSkillId; }
	FString GetSelectedTargetName() const { return _selectedTargetName; }
	int64 GetSelectedTargetWorldId() const { return _selectedTargetWorldId; }
	int64 GetSelectedTargetHandle() const { return _selectedTargetHandle; }
	FString GetActionFeedback() const { return _actionFeedback; }
	bool BuildSkillHudPresentation(FCorsairsSkillHudPresentation& Out) const;

#if WITH_DEV_AUTOMATION_TESTS
	void PressRightMouseForTests(FVector2D Cursor);
	void ReleaseRightMouseForTests(bool bUnusedAdvanceTime);
	bool RotateCameraForTests(float MouseX);
	void ResetCameraYawForTests();
	void ApplyWheelForTests(float WheelDelta);
	void PrepareShortcutForTests(int32 Slot) { HandleShortcut(Slot); }
	void SelectActorForTests(const FCorsairsWorldActor& Actor);
	void ActorLeftForTests(int64 WorldId) { HandleActorLeft(WorldId); }
	void SubmitIntentForTests(
		const FCorsairsClickIntent& Intent,
		ECorsairsPathMode PathMode = ECorsairsPathMode::Normal);
	void HandleMovementForTests(const FCorsairsMovementEvent& Event)
	{
		HandleMovementChanged(Event);
	}
	bool HasPendingNpcForTests() const { return _pendingNpc.IsSet(); }
	bool HasContinuationForTests() const { return _continuationIntent.IsSet(); }
	void SeedContinuationForTests(const FCorsairsClickIntent& Intent)
	{
		_continuationIntent = FCorsairsQueuedClickIntent{
			Intent,
			ECorsairsPathMode::Normal,
		};
	}
#endif

private:
	void ApplyFreeCursorMode();
	void ApplyCapturedCursorMode();
	bool IsLoginUiAcceptingInput() const;
	void HandleTypedKey(FKey Key);

	void HandleLeftPressed();
	void HandleLeftReleased();
	void HandleRightPressed();
	void HandleRightReleased();
	void HandleRightDoubleClick();
	void HandleMouseX(float Value);
	void HandleMouseY(float Value);
	void HandleMouseWheel(float Value);
	void HandleShortcut(int32 Slot);

	bool BuildCursorSnapshot(FCorsairsClickInput& OutInput) const;
	bool TraceExactActor(
		const FVector& RayOrigin,
		const FVector& RayDirection,
		FCorsairsWorldActor& OutActor) const;
	bool TryFindExactActor(
		int64 WorldId,
		int64 Handle,
		FCorsairsWorldActor& OutActor) const;
	const FCorsairsSkillEntry* FindUsableSkill(int64 SkillId) const;
	void ResolveCursorClick(ECorsairsPathMode PathMode, bool bHeldWalkOnly);
	void SubmitIntent(
		const FCorsairsClickIntent& Intent,
		ECorsairsPathMode PathMode);
	void ExecuteIntent(const FCorsairsQueuedClickIntent& Queued);
	bool SendMovementToward(
		FIntPoint Target,
		ECorsairsPathMode PathMode,
		const TOptional<FCorsairsQueuedClickIntent>& Continuation);
	TArray<FIntPoint> BuildApproachPath(
		FIntPoint Target,
		ECorsairsPathMode PathMode,
		FCorsairsPathResult& OutResult) const;
	void ExecuteTalk(const FCorsairsClickIntent& Intent);
	void CancelTargeting(bool bRequestServerCancel);
	void ResetGameplayState();
	void SetFeedback(const FString& Message);

	UFUNCTION()
	void HandleMovementChanged(const FCorsairsMovementEvent& Event);

	UFUNCTION()
	void HandleActorLeft(int64 WorldId);

	UFUNCTION()
	void HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message);

	UFUNCTION()
	void HandleSkillStateChanged();

	UFUNCTION()
	void HandleProtocolError(const FString& Message);

	UPROPERTY()
	TObjectPtr<UCorsairsSession> _session;

	const FCorsairsCharacterGround* _ground = nullptr;
	const FCorsairsSkillCatalog* _skillCatalog = nullptr;
	FCorsairsMouseControlState _mouseState;
	TOptional<FCorsairsQueuedClickIntent> _pendingReplacement;
	TOptional<FCorsairsQueuedClickIntent> _continuationIntent;
	TOptional<FCorsairsPendingNpcInteraction> _pendingNpc;
	int64 _preparedSkillId = 0;
	double _cameraZoom = 1.0;
	FString _selectedTargetName;
	int64 _selectedTargetWorldId = 0;
	int64 _selectedTargetHandle = 0;
	FString _actionFeedback;
	bool _inputBindingsInstalled = false;
};
