#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"

#include "CorsairsGameInstance.generated.h"

// Держит сессию и соединение поверх смены уровней: при переходе между картами
// UGameInstance переживает загрузку, в отличие от GameMode и уровня.
UCLASS()
class CORSAIRSGAME_API UCorsairsGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;
};
