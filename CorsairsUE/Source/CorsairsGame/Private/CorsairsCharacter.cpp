#include "CorsairsCharacter.h"

#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CorsairsCharacterGround.h"
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
	PrimaryActorTick.bCanEverTick = true;
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

void ACorsairsCharacter::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!_serverPathFollower.IsActive() ||
		!FMath::IsFinite(_serverMovementSpeedCmPerSecond) ||
		_serverMovementSpeedCmPerSecond <= 0.0)
	{
		return;
	}

	const FIntPoint SourcePosition = _serverPathFollower.Advance(
		_serverMovementSpeedCmPerSecond * static_cast<double>(DeltaSeconds));
	ApplyServerPathPosition(SourcePosition, true);
}

void ACorsairsCharacter::AttachCharacterGround(
	const FCorsairsCharacterGround* InGround)
{
	CharacterGround = InGround;
}

void ACorsairsCharacter::HandleServerMovementChanged(
	const FCorsairsMovementEvent& Event)
{
	if (Event.Type == ECorsairsMovementEventType::AcceptedPath)
	{
		if (!Event.bServerDriven ||
			!FMath::IsFinite(Event.MovementSpeedCmPerSecond) ||
			Event.MovementSpeedCmPerSecond <= 0.0)
		{
			StopServerPathFollower();
			return;
		}

#if WITH_DEV_AUTOMATION_TESTS
		++_serverPathAcceptCount;
#endif
		_serverPathFollower.Accept(Event.Waypoints);
		_serverMovementSpeedCmPerSecond = Event.MovementSpeedCmPerSecond;
		ApplyServerPathPosition(_serverPathFollower.GetPosition(), true);
		return;
	}

	_serverPathFollower.Reconcile(Event.Endpoint);
	_serverMovementSpeedCmPerSecond = 0.0;
	ApplyServerPathPosition(_serverPathFollower.GetPosition(), false);
	_serverPathFollower.Stop();
}

void ACorsairsCharacter::StopServerPathFollower()
{
	_serverPathFollower.Stop();
	_serverMovementSpeedCmPerSecond = 0.0;
}

void ACorsairsCharacter::ApplyServerPathPosition(
	const FIntPoint SourcePosition,
	const bool bApplyFacing)
{
	FVector Location = GetActorLocation();
	Location.X = -SourcePosition.Y;
	Location.Y = SourcePosition.X;
	if (CharacterGround != nullptr && CharacterGround->IsLoaded())
	{
		Location = CharacterGround->ActorCenter(
			SourcePosition,
			GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	}

	if (bApplyFacing)
	{
		SetActorLocationAndRotation(
			Location,
			FRotator(0.0, _serverPathFollower.GetFacingYaw(), 0.0),
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
		return;
	}

	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
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

	// Делитель ограничен снизу осмысленным ростом, а не единицей. Прежний
	// кламп к 1.0 превращал частичную сборку в катастрофу: когда части тела
	// не загрузились и мерить нечего, рост сборки равнялся нулю, делитель —
	// единице, и персонаж раздувался в CharacterHeight раз — глыба в сотни
	// метров растянутых текселей во весь экран. Сборка ниже полуметра — это
	// не карлик, это отказ загрузки частей; масштабировать её бессмысленно,
	// и молчать о ней нельзя.
	constexpr double MinPlausibleHeight = 50.0;
	if (CombinedHeight < MinPlausibleHeight)
	{
		UE_LOG(LogCorsairsCharacter, Error,
			   TEXT("сборка ростом %.1f см — части не загрузились, масштаб не применён"),
			   CombinedHeight);
		GetMesh()->SetRelativeScale3D(FVector(1.0));
		return;
	}

	const double Factor = CharacterHeight / CombinedHeight;
	GetMesh()->SetRelativeScale3D(FVector(Factor));
}
