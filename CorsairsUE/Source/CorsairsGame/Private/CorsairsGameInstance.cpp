#include "CorsairsGameInstance.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsGame, Log, All);

void UCorsairsGameInstance::Init()
{
	Super::Init();
	UE_LOG(LogCorsairsGame, Log, TEXT("CorsairsGameInstance: инициализация"));
}

void UCorsairsGameInstance::Shutdown()
{
	UE_LOG(LogCorsairsGame, Log, TEXT("CorsairsGameInstance: завершение"));
	Super::Shutdown();
}
