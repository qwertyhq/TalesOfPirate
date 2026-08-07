#pragma once

#include "CoreMinimal.h"
#include "CorsairsCharacterCatalog.h"
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

	bool ApplyAppearance(const FCorsairsResolvedAppearance& Appearance);
	USkeletalMeshComponent* GetPartComponent(int32 Slot) const;
	int32 GetVisiblePartCount() const { return VisibleParts.Num(); }
	virtual void AttachCharacterGround(
		const FCorsairsCharacterGround* InGround);

protected:
	bool PlayAppearanceAnimation(const FSoftObjectPath& AnimationPath);
	void HideVisibleParts();
	void ApplySharedScale();
	const FCorsairsCharacterGround* GetCharacterGround() const
	{
		return CharacterGround;
	}

	const FCorsairsCharacterGround* CharacterGround = nullptr;

private:
	UPROPERTY(VisibleAnywhere, Category = "Corsairs")
	TArray<TObjectPtr<USkeletalMeshComponent>> VisibleParts;
};
