#include "CorsairsCharacter.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsCharacter, Log, All);

namespace
{
	constexpr double CharacterHeight = 176.0;

	UAnimSequence* LoadAppearanceAnimation(
		const FSoftObjectPath& AnimationPath)
	{
		UAnimSequence* Sequence =
			Cast<UAnimSequence>(AnimationPath.TryLoad());
		if (Sequence == nullptr)
		{
			UE_LOG(
				LogCorsairsCharacter,
				Warning,
				TEXT("анимация не загрузилась: %s"),
				*AnimationPath.ToString());
		}
		return Sequence;
	}

	void PlayLoopingAnimation(
		USkeletalMeshComponent* Mesh,
		UAnimSequence* Sequence)
	{
		Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Mesh->PlayAnimation(Sequence, true);
	}
}

ACorsairsCharacter::ACorsairsCharacter()
{
	GetMesh()->VisibilityBasedAnimTickOption =
		EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	GetMesh()->SetRelativeLocation(FVector(0.0, 0.0, -88.0));
	GetMesh()->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));

	const TStaticArray<FName, 5> PartNames = {
		TEXT("Head"),
		TEXT("Face"),
		TEXT("Body"),
		TEXT("Gloves"),
		TEXT("Shoes")
	};
	VisibleParts.Reserve(PartNames.Num());
	for (const FName PartName : PartNames)
	{
		USkeletalMeshComponent* Part =
			CreateDefaultSubobject<USkeletalMeshComponent>(PartName);
		Part->SetupAttachment(GetMesh());
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		VisibleParts.Add(Part);
	}
}

bool ACorsairsCharacter::ApplyAppearance(
	const FCorsairsResolvedAppearance& Appearance)
{
	if (Appearance.bModular)
	{
		USkeletalMesh* Driver =
			Cast<USkeletalMesh>(Appearance.DriverMesh.TryLoad());
		if (Driver == nullptr)
		{
			UE_LOG(
				LogCorsairsCharacter,
				Warning,
				TEXT("driver mesh не загрузился: %s"),
				*Appearance.DriverMesh.ToString());
			return false;
		}

		UAnimSequence* Animation =
			LoadAppearanceAnimation(Appearance.Animation);
		if (Animation == nullptr)
		{
			return false;
		}

		TStaticArray<USkeletalMesh*, 5> PartMeshes = {
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		for (int32 Slot = 0; Slot < VisibleParts.Num(); ++Slot)
		{
			const FSoftObjectPath& PartPath = Appearance.PartMeshes[Slot];
			if (PartPath.IsNull())
			{
				continue;
			}

			PartMeshes[Slot] =
				Cast<USkeletalMesh>(PartPath.TryLoad());
			if (PartMeshes[Slot] == nullptr)
			{
				UE_LOG(
					LogCorsairsCharacter,
					Warning,
					TEXT("часть %d не загрузилась: %s"),
					Slot,
					*PartPath.ToString());
			}
		}

		GetMesh()->SetSkeletalMesh(Driver);
		GetMesh()->VisibilityBasedAnimTickOption =
			EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		GetMesh()->SetVisibility(false, false);

		for (int32 Slot = 0; Slot < VisibleParts.Num(); ++Slot)
		{
			USkeletalMeshComponent* Part = VisibleParts[Slot];
			Part->SetSkeletalMesh(PartMeshes[Slot]);
			Part->SetLeaderPoseComponent(GetMesh(), true, false);
			Part->SetVisibility(true, false);
		}

		ApplySharedScale();
		PlayLoopingAnimation(GetMesh(), Animation);
		return true;
	}

	USkeletalMesh* Mesh =
		Cast<USkeletalMesh>(Appearance.StaticMesh.TryLoad());
	if (Mesh == nullptr)
	{
		UE_LOG(
			LogCorsairsCharacter,
			Warning,
			TEXT("модель персонажа не загрузилась: %s"),
			*Appearance.StaticMesh.ToString());
		return false;
	}

	UAnimSequence* Animation =
		LoadAppearanceAnimation(Appearance.Animation);
	if (Animation == nullptr)
	{
		return false;
	}

	HideVisibleParts();
	GetMesh()->SetSkeletalMesh(Mesh);
	GetMesh()->SetVisibility(true, false);
	ApplySharedScale();
	PlayLoopingAnimation(GetMesh(), Animation);
	return true;
}

USkeletalMeshComponent* ACorsairsCharacter::GetPartComponent(
	const int32 Slot) const
{
	return VisibleParts.IsValidIndex(Slot) ? VisibleParts[Slot] : nullptr;
}

bool ACorsairsCharacter::PlayAppearanceAnimation(
	const FSoftObjectPath& AnimationPath)
{
	UAnimSequence* Sequence = LoadAppearanceAnimation(AnimationPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	PlayLoopingAnimation(GetMesh(), Sequence);
	return true;
}

void ACorsairsCharacter::HideVisibleParts()
{
	for (USkeletalMeshComponent* Part : VisibleParts)
	{
		Part->SetSkeletalMesh(nullptr);
		Part->SetLeaderPoseComponent(nullptr, true, false);
		Part->SetVisibility(false, false);
	}
}

void ACorsairsCharacter::ApplySharedScale()
{
	FBox CombinedBounds(EForceInit::ForceInit);
	if (GetMesh()->IsVisible())
	{
		if (const USkeletalMesh* Mesh = GetMesh()->GetSkeletalMeshAsset())
		{
			CombinedBounds += Mesh->GetBounds().GetBox();
		}
	}
	else
	{
		for (const USkeletalMeshComponent* Part : VisibleParts)
		{
			if (const USkeletalMesh* Mesh = Part->GetSkeletalMeshAsset())
			{
				CombinedBounds += Mesh->GetBounds().GetBox();
			}
		}
	}

	const double CombinedHeight =
		CombinedBounds.IsValid ? CombinedBounds.GetSize().Z : 0.0;
	const double Factor = CharacterHeight / FMath::Max(CombinedHeight, 1.0);
	GetMesh()->SetRelativeScale3D(FVector(Factor));
}
