#pragma once

#include "CoreMinimal.h"
#include "CorsairsActionReducer.h"
#include "CorsairsCharacterCatalog.h"
#include "CorsairsServerPathFollower.h"
#include "GameFramework/Character.h"

#include "CorsairsCharacter.generated.h"

class USkeletalMeshComponent;
class FCorsairsCharacterGround;

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

	UPROPERTY(VisibleAnywhere, Category = "Corsairs")
	TArray<TObjectPtr<USkeletalMeshComponent>> VisibleParts;
};
