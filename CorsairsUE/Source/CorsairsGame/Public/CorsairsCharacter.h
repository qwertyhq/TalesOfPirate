#pragma once

#include "CoreMinimal.h"
#include "CorsairsActionReducer.h"
#include "CorsairsCharacterCatalog.h"
#include "CorsairsServerPathFollower.h"
#include "GameFramework/Character.h"

#include "CorsairsCharacter.generated.h"

class USkeletalMeshComponent;
class FCorsairsCharacterGround;

/** Серверный адрес и тип персонажа для gameplay-запросов из world hit. */
USTRUCT(BlueprintType)
struct CORSAIRSGAME_API FCorsairsServerIdentity
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 WorldId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 Handle = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 CtrlType = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 ChaId = 0;

	bool operator==(const FCorsairsServerIdentity&) const = default;
};

UCLASS()
class CORSAIRSGAME_API ACorsairsCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ACorsairsCharacter();
	virtual void Tick(float DeltaSeconds) override;

	bool ApplyAppearance(const FCorsairsResolvedAppearance& Appearance);
	USkeletalMeshComponent* GetPartComponent(int32 Slot) const;
	int32 GetVisiblePartCount() const { return VisibleParts.Num(); }
	virtual void AttachCharacterGround(
		const FCorsairsCharacterGround* InGround);
	void HandleServerMovementChanged(const FCorsairsMovementEvent& Event);
	bool InitializeServerIdentity(const FCorsairsServerIdentity& identity);

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	bool TryGetServerIdentity(FCorsairsServerIdentity& outIdentity) const;

#if WITH_DEV_AUTOMATION_TESTS
	int32 GetServerPathAcceptCountForTests() const
	{
		return _serverPathAcceptCount;
	}
#endif

protected:
	bool PlayAppearanceAnimation(const FSoftObjectPath& AnimationPath);
	void HideVisibleParts();
	void ApplySharedScale();
	void StopServerPathFollower();
	const FCorsairsCharacterGround* GetCharacterGround() const
	{
		return CharacterGround;
	}

	const FCorsairsCharacterGround* CharacterGround = nullptr;
	FCorsairsServerPathFollower _serverPathFollower;
	double _serverMovementSpeedCmPerSecond = 0.0;

#if WITH_DEV_AUTOMATION_TESTS
	int32 _serverPathAcceptCount = 0;
#endif

private:
	void ApplyServerPathPosition(FIntPoint SourcePosition, bool bApplyFacing);

	UPROPERTY()
	FCorsairsServerIdentity _serverIdentity;

	bool _hasServerIdentity = false;

	UPROPERTY(VisibleAnywhere, Category = "Corsairs")
	TArray<TObjectPtr<USkeletalMeshComponent>> VisibleParts;
};
